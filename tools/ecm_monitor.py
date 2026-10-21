#!/usr/bin/env python3
"""Long-running passive monitor for the ECM <-> Wi-Fi-module serial bus.

LISTEN-ONLY. Opens the logic analyzer for capture only. Nothing here transmits
on the bus, and there is no code path that could.

WHY THIS EXISTS
---------------
`sigrok-cli` cannot be trusted to run for more than ~10 minutes on this setup.
Measured: 1 MHz died after 457 s (457.8 M samples), 250 kHz after 651 s
(162.7 M samples). Lowering the rate bought only ~40% more time, so it is NOT a
sample budget — it behaves like an unreliable dropout, and it fails SILENTLY:
`run.sr` is valid right up to the moment it stops.

A single long capture therefore ends early and you may not notice. So this runs
sigrok in SHORT SEGMENTS back to back, and polls /poll INDEPENDENTLY of the
segments — so a module reboot is caught by the read path even if it lands in the
brief gap between two segments.

The read path matters more than the wire here: a module reboot and the ordinary
post-command stall are INDISTINGUISHABLE on the bus (both are ~5 s of silence
with the ECM polling through). Only `uptime` resetting tells them apart.

Usage:
    ./ecm_monitor.py --hours 8                 # 8 hours, 300 s segments
    ./ecm_monitor.py --minutes 30 --segment 120
    ./ecm_monitor.py --hours 2 --analyze       # also parse each segment on the fly
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(HERE, "tools"))

DEFAULT_HOST = "fireplace.local"
#: Poll cadence. NEVER below 5 s — at 0.5 s the module's HTTP server has been
#: observed to stop answering entirely and not recover within a capture.
DEFAULT_POLL_S = 5.0
#: Segment length. Well inside the ~8-11 minute observed dropout limit.
DEFAULT_SEGMENT_S = 300.0
DEFAULT_SAMPLERATE = "250k"          # 26 samples/bit at 9600 baud: ample

BANNER = """
LISTEN-ONLY. Nothing in this monitor transmits on the bus.

  * Analyzer GND connects FIRST and is removed LAST.
  * NEVER connect the analyzer to ECM pin 3 (+5 V supply).
  * Analyzer GND -> ECM pin 2 or 8, CH0 -> pin 6, CH1 -> pin 4.
  * Selector in IFT-REM means the port is powered AND THE FIREPLACE CAN IGNITE.
  * Keep the RF remote's batteries OUT for the whole session.
  * The analyzer is powered from laptop USB only - never from the fireplace.
  * This monitor never calls POST /post - that is the write path.
"""


def sigrok_cli() -> str:
    """Locate sigrok-cli. Must be absolute-usable: see the path note below."""
    for c in (shutil.which("sigrok-cli"),
              os.path.expanduser("~/.local/bin/sigrok-cli"),
              "/Applications/sigrok-cli.app/Contents/MacOS/sigrok-cli"):
        if c and os.path.exists(c):
            return c
    raise SystemExit("sigrok-cli not found")


def analyzer_present() -> bool:
    try:
        r = subprocess.run([sigrok_cli(), "--scan"], capture_output=True,
                           text=True, timeout=30)
    except Exception:
        return False
    return "fx2lafw" in r.stdout.lower()


def poll(host: str, timeout: float = 5.0) -> tuple[dict | None, str, float]:
    """GET /poll. READ-ONLY. Never call /post — that is the write path.

    Returns ``(payload, outcome, latency_s)`` rather than just the payload.

    WHY THE OUTCOME MATTERS. This used to return ``None`` for every failure, so
    a gap in polls.jsonl conflated at least four different situations: the
    module refusing the connection, the module hanging without answering, the
    request timing out, and the poller's own thread being starved. That made a
    real finding unattributable — a run showed HTTP dropping out for 15-125 s
    while the module's UART kept running at its normal rate, and the data could
    not say whether the module or the laptop was at fault.

    Outcomes: ok, timeout, refused, unreachable, http_<code>, bad_json, <ExcName>.
    """
    import socket
    import urllib.error
    import urllib.request

    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(f"http://{host}/poll", timeout=timeout) as r:
            body = r.read().decode()
            lat = time.monotonic() - t0
            try:
                return json.loads(body), "ok", lat
            except Exception:
                return None, "bad_json", lat
    except urllib.error.HTTPError as exc:
        return None, f"http_{exc.code}", time.monotonic() - t0
    except urllib.error.URLError as exc:
        reason = getattr(exc, "reason", None)
        lat = time.monotonic() - t0
        if isinstance(reason, socket.timeout):
            return None, "timeout", lat
        # ConnectionRefusedError means something IS there and said no — a
        # different situation from a host that cannot be reached at all.
        if isinstance(reason, ConnectionRefusedError):
            return None, "refused", lat
        return None, f"unreachable:{type(reason).__name__}", lat
    except socket.timeout:
        return None, "timeout", time.monotonic() - t0
    except Exception as exc:
        return None, type(exc).__name__, time.monotonic() - t0


def default_gateway() -> str | None:
    """Best-effort default-gateway address, used as the CONTROL probe.

    Needed because a probe of the module alone is ambiguous at the bottom end:
    "the module does not answer ping" is produced both by the module being
    off/de-powered AND by this laptop having lost the network. Probing a second,
    independent host is what separates them.
    """
    try:
        if sys.platform == "darwin":
            r = subprocess.run(["route", "-n", "get", "default"],
                               capture_output=True, text=True, timeout=5)
            for line in r.stdout.splitlines():
                if "gateway:" in line:
                    return line.split("gateway:")[1].strip()
        else:
            r = subprocess.run(["ip", "route", "show", "default"],
                               capture_output=True, text=True, timeout=5)
            for tok in r.stdout.split():
                if tok.count(".") == 3:
                    return tok
    except Exception:
        pass
    return None


def ping_once(host: str, timeout_s: float = 2.0) -> tuple[bool, float]:
    """One ICMP echo. Returns (reachable, latency_s).

    Run in PARALLEL with /poll, and it is the control that separates two very
    different failures. The module's TCP/IP stack and its HTTP server are not
    the same thing: if ping answers while /poll fails, the network is up and the
    HTTP server specifically is the problem. If both fail, the whole device is
    down. Without this, the two are indistinguishable in the logs.

    Treated as best-effort — unprivileged ICMP is unavailable on some setups and
    a failure to probe must never affect the capture.
    """
    if sys.platform == "darwin":
        cmd = ["ping", "-c", "1", "-t", str(int(timeout_s))]
    else:
        cmd = ["ping", "-c", "1", "-W", str(int(timeout_s))]
    t0 = time.monotonic()
    try:
        r = subprocess.run(cmd + [host], capture_output=True, text=True,
                           timeout=timeout_s + 2)
        return r.returncode == 0, time.monotonic() - t0
    except Exception:
        return False, time.monotonic() - t0


class PollLogger(threading.Thread):
    """Samples /poll for the whole session, across segment boundaries.

    This is the REBOOT DETECTOR: the module's `uptime` only ever rises, so a
    decrease is a reboot. The wire cannot do this job — a reboot and a stall
    look identical on the bus.

    It writes TWO files, and the second one is the point:

      polls.jsonl          successful samples only, as before
      poll_attempts.jsonl  EVERY attempt: outcome, latency, and whether ICMP
                           answered. Without this, a gap in polls.jsonl cannot
                           be attributed — see the note on `poll()`.
    """

    def __init__(self, host: str, interval: float, path: str,
                 attempts_path: str | None = None, do_ping: bool = True,
                 control_host: str | None = None) -> None:
        super().__init__(daemon=True)
        self.host, self.interval, self.path = host, interval, path
        self.attempts_path = attempts_path
        self.do_ping = do_ping
        #: The control. Without it, "module unreachable" is ambiguous between
        #: the module being down and this laptop being off the network.
        self.control_host = control_host
        # NOT named `_stop`: threading.Thread has an internal _stop() that
        # join() calls, and shadowing it raises a TypeError.
        self._halt = threading.Event()
        self.samples: list[tuple[float, dict]] = []

    def run(self) -> None:
        t0 = time.monotonic()
        while not self._halt.is_set():
            d, outcome, latency = poll(self.host)
            t = time.monotonic() - t0
            if d is not None:
                self.samples.append((t, d))
                with open(self.path, "a") as fh:
                    fh.write(json.dumps({"t": round(t, 3), "poll": d}) + "\n")

            if self.attempts_path:
                rec = {"t": round(t, 3), "outcome": outcome,
                       "latency_s": round(latency, 3)}
                if self.do_ping:
                    # Only worth probing once HTTP has NOT answered: a healthy
                    # /poll already proves the stack is up, and this keeps the
                    # probe off the happy path entirely.
                    if outcome == "ok":
                        rec["ping"] = "skipped"
                        rec["control"] = "skipped"
                    else:
                        up, plat = ping_once(self.host)
                        rec["ping"] = "ok" if up else "fail"
                        rec["ping_s"] = round(plat, 3)
                        if self.control_host:
                            # The control probe. Its job is to fail when our own
                            # networking is the problem, so that a module that is
                            # merely OFF is not misread as a module that is
                            # broken, and vice versa.
                            cup, clat = ping_once(self.control_host)
                            rec["control"] = "ok" if cup else "fail"
                            rec["control_s"] = round(clat, 3)
                try:
                    with open(self.attempts_path, "a") as fh:
                        fh.write(json.dumps(rec) + "\n")
                except Exception:
                    pass
            self._halt.wait(self.interval)

    def stop(self) -> None:
        self._halt.set()

    def reboots(self) -> list[dict]:
        out, prev = [], None
        for t, d in self.samples:
            u = d.get("uptime")
            if prev is not None and u is not None and u < prev:
                out.append({"t": round(t, 1), "from_s": prev, "to_s": u})
            prev = u
        return out


def run_segment(out_sr: str, seconds: float, samplerate: str, log_path: str) -> dict:
    """Run ONE sigrok capture. Returns what happened, including whether it died early."""
    # ABSOLUTE path: the sigrok-cli wrapper cd's into its app bundle, so a
    # relative -o fails with "Not a regular file".
    out_sr = os.path.abspath(out_sr)
    cmd = [sigrok_cli(), "--driver", "fx2lafw",
           "--config", f"samplerate={samplerate}",
           "--channels", "D0,D1",
           "--time", f"{int(seconds)}s",          # integer + unit: a float or a
           "--output-file", out_sr]               # bare number is rejected/ms
    started = time.monotonic()
    # Record where this segment's own log output begins BEFORE running it.
    start_pos = os.path.getsize(log_path) if os.path.exists(log_path) else 0
    with open(log_path, "a") as lf:
        lf.write("$ " + " ".join(cmd) + "\n")
        subprocess.run(cmd, stdout=lf, stderr=subprocess.STDOUT)
    elapsed = time.monotonic() - started

    # Did it run its full length? sigrok reports the shortfall in its own log.
    #
    # ⚠️ Read only THIS segment's output, from start_pos. This previously read
    # the last 3 lines of the SHARED log, so a single segment's
    # "Device only sent N samples." stayed in the tail and marked every
    # SUBSEQUENT segment short — a full-length 300 s segment was reported as
    # truncated, twice, and the false flag was escalated as a finding.
    # Duration is the other half of the check and is trustworthy.
    short = False
    note = ""
    try:
        with open(log_path) as fh:
            fh.seek(start_pos)
            own = fh.read()
        for line in own.splitlines():
            if "only sent" in line:
                short = True
                note = line.strip()
    except Exception:
        pass
    if elapsed < seconds * 0.95:
        short = True
        note = note or f"exited after {elapsed:.0f}s of {seconds:.0f}s"

    return {"file": out_sr, "requested_s": seconds, "elapsed_s": round(elapsed, 1),
            "short": short, "note": note, "bytes": os.path.getsize(out_sr)
            if os.path.exists(out_sr) else 0}


def read_attempts(path: str) -> list[dict]:
    """Load poll_attempts.jsonl. Tolerates a missing file and a half-written
    final line (the poller is a daemon thread and may be cut mid-write)."""
    out: list[dict] = []
    try:
        with open(path) as fh:
            for line in fh:
                try:
                    out.append(json.loads(line))
                except Exception:
                    continue
    except FileNotFoundError:
        pass
    return out


def summarise_attempts(attempts: list[dict]) -> dict:
    """Turn per-attempt records into the numbers that settle attribution.

    Three probes together, and the TERNARY is the whole point:

      module ping OK,  control OK    -> the module's HTTP server specifically is
                                        down; its network stack is up
      module ping FAIL, control OK   -> the module is off the LAN entirely
                                        (de-powered, hung, or mid-boot)
      module ping FAIL, control FAIL -> OUR network is down; says nothing about
                                        the module at all
    """
    outcomes: dict[str, int] = {}
    verdict: dict[str, int] = {}
    for a in attempts:
        o = a.get("outcome", "?")
        outcomes[o] = outcomes.get(o, 0) + 1
        if o == "ok" or a.get("ping") not in ("ok", "fail"):
            continue
        ctl = a.get("control")
        if ctl is None:
            v = "http_down" if a["ping"] == "ok" else "module_unreachable"
        elif a["ping"] == "ok":
            v = "http_down_stack_up"
        elif ctl == "ok":
            v = "module_off_lan"
        else:
            v = "our_network_down"
        verdict[v] = verdict.get(v, 0) + 1
    return {"outcomes": outcomes, "ping_during_failure": verdict}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--hours", type=float)
    g.add_argument("--minutes", type=float)
    ap.add_argument("--segment", type=float, default=DEFAULT_SEGMENT_S,
                    help=f"segment length in seconds (default {DEFAULT_SEGMENT_S:.0f})")
    ap.add_argument("--gap", type=float, default=3.0,
                    help="seconds to wait between segments (default 3). The device "
                         "CANNOT be reopened immediately after a previous sigrok-cli "
                         "exits — it fails with LIBUSB_ERROR_ACCESS / 'Failed to "
                         "detach kernel driver' until the kernel driver is released.")
    ap.add_argument("--max-consecutive-failures", type=int, default=5,
                    help="stop after this many segments in a row fail to open")
    ap.add_argument("--samplerate", default=DEFAULT_SAMPLERATE)
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--poll-interval", type=float, default=DEFAULT_POLL_S)
    ap.add_argument("--outdir", default=os.path.join(HERE, "captures"))
    ap.add_argument("--analyze", action="store_true",
                    help="parse each segment as it finishes (uses tools/ecm_frames.py). "
                         "⚠️ EXPENSIVE — measured at ~9 s per 300 s segment, and it "
                         "runs in the gap between segments, so it both widens the "
                         "blind window and loads the host. On one run that cost "
                         "9-27 s of unobserved wire per boundary. Prefer decoding "
                         "AFTER the session with tools/ecm_session.py.")
    ap.add_argument("--no-ping", action="store_true",
                    help="disable the parallel ICMP probe used to attribute /poll "
                         "failures (see poll_attempts.jsonl)")
    ap.add_argument("--control-host", default=None,
                    help="second host to ping as a CONTROL, so 'module down' can be "
                         "told apart from 'this laptop's network is down'. "
                         "Defaults to the default gateway.")
    args = ap.parse_args()

    total_s = (args.hours * 3600 if args.hours
               else args.minutes * 60 if args.minutes else 3600.0)

    print(BANNER)
    if not analyzer_present():
        print("ABORT: no fx2lafw analyzer detected. Is it plugged in?")
        return 2
    if poll(args.host)[0] is None:
        print(f"WARNING: /poll unreachable at {args.host} right now.")
        print("         The monitor still records the wire, but REBOOT DETECTION")
        print("         depends on /poll. Continuing — it may come back.")

    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    session = os.path.join(os.path.abspath(args.outdir), f"monitor-{stamp}")
    os.makedirs(session, exist_ok=True)
    polls_path = os.path.join(session, "polls.jsonl")
    attempts_path = os.path.join(session, "poll_attempts.jsonl")
    manifest_path = os.path.join(session, "manifest.jsonl")
    log_path = os.path.join(session, "capture.log")

    print(f"session dir : {session}")
    print(f"duration    : {total_s/3600:.2f} h in {args.segment:.0f}s segments "
          f"({int(total_s/args.segment)+1} expected)")
    print(f"samplerate  : {args.samplerate}")
    print(f"poll cadence: {args.poll_interval:g}s  -> {polls_path}")
    print(f"poll attempts: every attempt + ping -> {attempts_path}")
    control = args.control_host or (default_gateway() if not args.no_ping else None)
    if control:
        print(f"ping control : {control}  (distinguishes module-down from "
              f"our-network-down)")
    elif not args.no_ping:
        print("ping control : NONE — could not detect a gateway, so 'module "
              "unreachable' will be ambiguous")
    if args.analyze:
        print("⚠️  --analyze is on: it widens the inter-segment blind window.")
    print("\nCtrl-C to stop early (state is written as it goes).\n")

    poller = PollLogger(args.host, args.poll_interval, polls_path,
                        attempts_path=attempts_path, do_ping=not args.no_ping,
                        control_host=control)
    poller.start()

    segments: list[dict] = []
    t_start = time.monotonic()
    stopping = {"flag": False}

    def on_signal(*_):
        stopping["flag"] = True
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    try:
        n = 0
        consecutive_failures = 0
        while (time.monotonic() - t_start) < total_s and not stopping["flag"]:
            remaining = total_s - (time.monotonic() - t_start)
            seg_len = min(args.segment, max(30.0, remaining))
            seg_file = os.path.join(session, f"seg-{n:03d}.sr")
            print(f"[{time.strftime('%H:%M:%S')}] segment {n:03d}  "
                  f"({seg_len:.0f}s, {remaining/60:.1f} min left)", flush=True)
            info = run_segment(seg_file, seg_len, args.samplerate, log_path)
            info["n"] = n
            info["wall"] = datetime.now(timezone.utc).isoformat()
            segments.append(info)
            with open(manifest_path, "a") as fh:
                fh.write(json.dumps(info) + "\n")

            # A segment that produced NOTHING in ~no time did not fail mid-run;
            # it failed to OPEN. That is the device not being released yet, and
            # it needs a back-off, not another immediate attempt — without this
            # the loop spins through dozens of instant failures.
            opened = not (info["short"] and info["elapsed_s"] < 1.0 and info["bytes"] == 0)
            if not opened:
                consecutive_failures += 1
                wait = min(30.0, 3.0 * consecutive_failures)
                print(f"    ! could not open the device — backing off {wait:.0f}s "
                      f"({consecutive_failures}/{args.max_consecutive_failures})")
                if consecutive_failures >= args.max_consecutive_failures:
                    if not analyzer_present():
                        print("    ! analyzer is not on the USB bus — stopping")
                        break
                    print("    ! The analyzer ENUMERATES but will not OPEN "
                          "(LIBUSB_ERROR_ACCESS / 'Failed to detach kernel driver').")
                    print("    ! This does NOT clear on its own — it needs a PHYSICAL")
                    print("      REPLUG of the analyzer. Stopping so you can do that,")
                    print("      then re-run; the session directory is kept.")
                    break
                time.sleep(wait)
                n += 1
                continue
            consecutive_failures = 0

            if info["short"]:
                print(f"    ! ended early: {info['note']}")
                if not analyzer_present():
                    print("    ! analyzer is GONE from USB — waiting up to 5 min")
                    for _ in range(60):
                        if stopping["flag"]:
                            break
                        time.sleep(5)
                        if analyzer_present():
                            print("    ! analyzer is back")
                            break
                    else:
                        print("    ! analyzer did not return — stopping")
                        break
            if args.analyze:
                try:
                    from ecm_frames import decode_channel
                    fr = decode_channel(seg_file, "D0")
                    print(f"    {len(fr)} CRC-valid frames on D0")
                except Exception as exc:
                    print(f"    (analyse failed: {exc})")
            # Let the kernel driver be released before the next open. Without
            # this the next segment fails with LIBUSB_ERROR_ACCESS.
            if args.gap > 0 and not stopping["flag"]:
                time.sleep(args.gap)
            n += 1
    finally:
        poller.stop()
        poller.join(timeout=5)

    # ---- summary ---------------------------------------------------------
    reboots = poller.reboots()
    attempts = read_attempts(attempts_path)
    summary = {
        "session": session,
        "segments": len(segments),
        "segments_short": sum(1 for s in segments if s["short"]),
        "poll_samples": len(poller.samples),
        "poll_attempts": len(attempts),
        "poll_outcomes": summarise_attempts(attempts),
        "reboots_detected": reboots,
        "wall_start": datetime.now(timezone.utc).isoformat(),
    }
    with open(os.path.join(session, "summary.json"), "w") as fh:
        json.dump(summary, fh, indent=2)

    print(f"\n{'=' * 62}")
    print(f"segments run      : {len(segments)} "
          f"({summary['segments_short']} ended early)")
    print(f"/poll samples     : {len(poller.samples)} of {len(attempts)} attempts")
    for outcome, n in sorted(summary["poll_outcomes"]["outcomes"].items(),
                             key=lambda kv: -kv[1]):
        if outcome != "ok":
            print(f"    failed {outcome:<22} {n}")
    if summary["poll_outcomes"].get("ping_during_failure"):
        print(f"    attribution of failures: "
              f"{summary['poll_outcomes']['ping_during_failure']}")
        print( "      http_down_stack_up => the HTTP server specifically, stack up")
        print( "      module_off_lan     => the module is off the network entirely")
        print( "      our_network_down   => OUR network; says nothing about the module")
    print(f"REBOOTS DETECTED  : {len(reboots)}")
    for r in reboots:
        print(f"   t={r['t']:>7.1f}s   uptime {r['from_s']}s -> {r['to_s']}s")
    print(f"\nsummary : {os.path.join(session, 'summary.json')}")
    print(f"manifest: {manifest_path}")
    print(f"polls   : {polls_path}")
    print(f"attempts: {attempts_path}")
    print("\nDecode the whole session with (AFTER the run — never during):")
    print(f"  python3 tools/ecm_session.py {session}")
    print("=" * 62)
    return 0


if __name__ == "__main__":
    sys.exit(main())
