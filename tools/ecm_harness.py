#!/usr/bin/env python3
"""Capture the ECM <-> WFM bus while firing known fireplace actions.

LISTEN-ONLY. Nothing in this harness transmits on the bus. The bus is driven
only by the fireplace's own control path, via Home Assistant - the same path
the app uses. No frame is ever injected.

    * Analyzer GND connects FIRST and is removed LAST.
    * NEVER clip the analyzer to ECM pin 3 (+5 V supply).
    * Selector in IFT-REM means the port is powered AND THE FIREPLACE CAN IGNITE.
    * Keep the RF remote's batteries OUT for the entire session.
    * Analyzer is powered from laptop USB only - never from the fireplace.
    * Mains is live in the cavity. ECM low-voltage side only.
    * This sweep ignites a real gas appliance repeatedly.
      Do not leave the first real sweep unsupervised.
    * On any abort: flame off, pilot restored, safety timer cancelled.

WHY BASELINE IS INSIDE THE CAPTURE
----------------------------------
.sr files carry no reliable wall-clock start time, so correlating "the moment I
fired the trigger" against frame time is guesswork - and with a ~178 ms poll
cadence and inter-frame gaps up to 613 ms, +/-150 ms of uncertainty is fatal.

So this harness never correlates trigger time to frame time. It captures a wide
window, fires the mark at a known OFFSET INTO the capture, and then compares
frames before the mark (baseline) against frames after it. The command frame is
found by CONTENT - the first frame that differs from the baseline - not by
timestamp. Timing becomes a sanity check instead of a correctness requirement,
which is what lets a window absorb cloud round-trip latency.

TRIGGER MODES
-------------
    external  nothing is fired by this process. The harness records when each
              mark came due and prints "MARK DUE NOW"; an operator - or an
              agent holding Home Assistant tools - fires the real action at
              that moment. Non-blocking, so the harness can run in the
              background while the operator works in parallel. This is the
              semi-automatic path.
    manual    print the instruction and block for Enter. For a supervised run
              at the keyboard (including the first ignition).
    noop      fire nothing. Validates the harness plumbing with zero risk.
    webhook   POST to a Home Assistant webhook. Fully unattended; requires the
              one-time HA script + automation to exist (set up via MCP).

Usage:
    ./ecm_harness.py --dry-run
    ./ecm_harness.py --marks 12:noop --mark-mode noop
    ./ecm_harness.py --duration 40 --marks 12:flame_on,30:flame_off --mark-mode external
    ./ecm_harness.py --matrix tools/ecm_sweep_matrix.json --mark-mode webhook \\
                     --webhook-url http://192.0.2.10:8123/api/webhook/XXXX
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
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_frames import decode_channel, PairTable  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The fireplace runs its own 4-hour safety timer, armed by HA 30 s after the
# flame reports on. A long sweep must not cross it, or an un-commanded
# switch.turn_off lands inside a capture window and poisons the diff.
SAFETY_TIMER = "timer.fireplace_safety_timer"
FLAME_SWITCH = "switch.intellifire_flame"
PILOT_SWITCH = "switch.intellifire_pilot_light"

DEFAULT_HOST = "fireplace.local"
DEFAULT_SAMPLERATE = "1m"
DEFAULT_CHANNELS = "D0,D1"
# /poll cadence. NOT the 0.5 s this started at: measured on hardware 2026-10-16,
# polling at 0.5 s (≈220 requests over a 110 s capture) while the module was
# handling a flame command made the module STOP ANSWERING entirely — the poll
# series ended at t=96.2 s and never resumed, and the read path is the answer
# key. At 60 s (≈3 requests) it survived the same command. 5 s keeps the
# reboot detector usable while staying far below the level that killed it.
DEFAULT_POLL_S = 5.0

# /poll fields worth watching. `uptime` is the reboot detector: it only ever
# rises, so a decrease means the module restarted mid-run.
POLL_WATCH = ["power", "pilot", "height", "uptime", "downtime", "errors",
              "prepurge", "hot", "power_vent", "thermostat", "setpoint",
              "timer", "timeremaining", "light", "fanspeed", "temperature"]

ACTIONS = ("noop", "flame_on", "flame_off", "height", "pilot_on", "pilot_off")


# --------------------------------------------------------------------------
# /poll — the read-path answer key
# --------------------------------------------------------------------------

def poll(host: str = DEFAULT_HOST, timeout: float = 5.0) -> dict | None:
    """GET /poll. READ-ONLY. Never call /post — that is the write path."""
    import urllib.request
    try:
        with urllib.request.urlopen(f"http://{host}/poll", timeout=timeout) as r:
            return json.loads(r.read().decode())
    except Exception:
        return None


def poll_with_retry(host: str = DEFAULT_HOST, tries: int = 6,
                    delay: float = 2.0) -> dict | None:
    """Poll, retrying a few times.

    The module goes briefly unresponsive while it applies a command and during
    the post-command counter stall, so a SINGLE failed /poll is not evidence
    that the read path is down. Aborting a whole run on one missed request is
    too brittle: it killed a 4-minute operator-driven capture at pre-flight
    while the module was busy, and the fireplace was perfectly healthy.
    """
    for i in range(tries):
        d = poll(host)
        if d is not None:
            return d
        if i < tries - 1:
            time.sleep(delay)
    return None


class Poller(threading.Thread):
    """Sample /poll on a fixed cadence for the duration of a run.

    The cadence is deliberate and constant. /poll exposes `ecm_latency`, which
    implies the module queries the ECM to answer - so polling may itself inject
    bus traffic. Running the idle control at the SAME cadence makes that
    artefact appear in both and subtract out.
    """

    def __init__(self, host: str, interval: float, out_path: str) -> None:
        super().__init__(daemon=True)
        self.host, self.interval, self.out_path = host, interval, out_path
        self.samples: list[tuple[float, dict]] = []
        # NOTE: do NOT name this `_stop`. threading.Thread has an internal
        # `_stop()` method that `join()` calls, and shadowing it with an Event
        # makes join() raise "TypeError: 'Event' object is not callable".
        self._halt = threading.Event()

    def run(self) -> None:
        t0 = time.monotonic()
        while not self._halt.is_set():
            d = poll(self.host)
            if d is not None:
                t = time.monotonic() - t0
                self.samples.append((t, d))
                with open(self.out_path, "a") as fh:
                    fh.write(json.dumps({"t": round(t, 3), "poll": d}) + "\n")
            self._halt.wait(self.interval)

    def stop(self) -> None:
        self._halt.set()

    def reboots(self) -> list[dict]:
        """Detect module reboots: uptime must never decrease."""
        out = []
        for i in range(1, len(self.samples)):
            a, b = self.samples[i - 1][1], self.samples[i][1]
            if a.get("uptime") is not None and b.get("uptime") is not None:
                if b["uptime"] < a["uptime"]:
                    out.append({"t": self.samples[i][0],
                                "from": a["uptime"], "to": b["uptime"]})
        return out

    def at(self, t: float) -> dict | None:
        """Nearest /poll sample to capture-relative time t."""
        if not self.samples:
            return None
        return min(self.samples, key=lambda s: abs(s[0] - t))[1]


# --------------------------------------------------------------------------
# marks and firers
# --------------------------------------------------------------------------

@dataclass
class Mark:
    at_s: float
    action: str
    value: int | None = None
    expect: dict | None = None
    note: str = ""

    @property
    def label(self) -> str:
        return f"{self.action}={self.value}" if self.value is not None else self.action


@dataclass
class MarkResult:
    mark: Mark
    due_at: float | None = None     # when the mark came due, capture-relative
    fired_at: float | None = None   # best estimate of when it was actually fired
    returned_at: float | None = None
    fired_wall: str | None = None
    poll_before: dict | None = None
    poll_after: dict | None = None
    verified: bool | None = None
    detail: str = ""


def parse_marks(spec: str, allow_unknown: bool = True) -> list[Mark]:
    """Parse '12:flame_on,30:flame_off,45:height=3' into Marks.

    An action OUTSIDE the vocabulary is accepted as a free-form LABEL when
    `allow_unknown` is set, with a warning. This used to hard-fail, which
    killed two real captures at startup over nothing: marks are only timing
    hints in `external` mode, and the operator fires the real action anyway.
    Losing a hardware session to a label typo is a bad trade.

    Actions outside the vocabulary are treated as `noop` by every firer, so
    they can never trigger anything.
    """
    marks = []
    for chunk in spec.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        at, _, act = chunk.partition(":")
        act = act.strip()
        value = None
        if "=" in act:
            act, _, v = act.partition("=")
            value = int(v)
        act = act.strip()
        if act not in ACTIONS:
            if not allow_unknown:
                raise SystemExit(
                    f"unknown action '{act}' (known: {', '.join(ACTIONS)})")
            print(f"# warning: '{act}' is not a known action "
                  f"({', '.join(ACTIONS)}); treating it as a label only — "
                  f"nothing will be fired for it", file=sys.stderr)
            marks.append(Mark(float(at), "noop", None, None,
                              f"free-form label: {act}"))
            continue
        marks.append(Mark(float(at), act, value))
    return sorted(marks, key=lambda m: m.at_s)


class Firer:
    """Fires fireplace actions. Subclasses implement the transport."""

    name = "base"

    def fire(self, mark: Mark) -> tuple[bool, str]:
        raise NotImplementedError

    def reset(self) -> None:
        """Best-effort return to a known state between runs."""


class NoopFirer(Firer):
    """Fires nothing. For validating the plumbing with zero risk."""
    name = "noop"

    def fire(self, mark: Mark) -> tuple[bool, str]:
        if mark.action != "noop":
            return True, "noop firer: action not sent by design"
        return True, "noop"


class ExternalFirer(Firer):
    """Fires nothing itself — something OUTSIDE this process does.

    This is the semi-automatic path. The harness records the capture-relative
    moment each mark came due; an operator (or an agent holding Home Assistant
    tools) fires the real action at that moment through whatever path it has.
    Nothing blocks, so the process can run in the background while the operator
    works in parallel.

    Because the diff is CONTENT-based — the first frame that differs from the
    baseline — the mark time only has to be approximately right. A second or
    two of asymmetry does not misattribute anything, which is why this works
    without a precise handoff.
    """
    name = "external"

    def fire(self, mark: Mark) -> tuple[bool, str]:
        print(f"\n>>> MARK DUE NOW: {mark.label}"
              f"{'  (expect ' + json.dumps(mark.expect) + ')' if mark.expect else ''}",
              flush=True)
        return True, "external — fired by the operator, not this process"


class ManualFirer(Firer):
    """Prints the instruction and waits for the operator.

    Blocks on stdin, so it cannot be driven by anything that cannot type into
    the process. For a supervised run at the keyboard.
    """
    name = "manual"

    def __init__(self, interactive: bool = True) -> None:
        self.interactive = interactive
        self._queue: list[str] = []

    def fire(self, mark: Mark) -> tuple[bool, str]:
        msg = f"FIRE NOW: {mark.label}"
        if mark.expect:
            msg += f"   (expect {mark.expect})"
        print(f"\n>>> {msg}", flush=True)
        if not self.interactive:
            return True, "non-interactive: operator must act now"
        if self._queue:
            return True, f"queued: {self._queue.pop(0)}"
        try:
            got = input("    press Enter once you have fired it: ")
        except EOFError:
            return True, "eof — assumed fired"
        if got.strip():
            self._queue.append(got.strip())
        return True, "confirmed by operator"


class WebhookFirer(Firer):
    """POST to a Home Assistant webhook -> HA script -> fireplace entity.

    Unattended. The webhook URL IS the credential (it is revocable and scoped
    to one automation), so it is never logged in full.
    """
    name = "webhook"

    def __init__(self, url: str, timeout: float = 5.0,
                 verify: bool = True, host: str = DEFAULT_HOST) -> None:
        self.url, self.timeout, self.verify, self.host = url, timeout, verify, host

    def fire(self, mark: Mark) -> tuple[bool, str]:
        import urllib.request
        body = json.dumps({"action": mark.action,
                           "value": mark.value if mark.value is not None else 0}).encode()
        req = urllib.request.Request(self.url, data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                code = r.status
        except Exception as exc:
            return False, f"webhook unreachable: {exc}"
        if code != 200:
            return False, f"webhook returned HTTP {code}"
        return True, "posted"

    def reset(self) -> None:
        """Best-effort return to a known safe state."""
        for act in ("flame_off", "pilot_on"):
            try:
                self.fire(Mark(0, act))
            except Exception:
                pass


# --------------------------------------------------------------------------
# capture
# --------------------------------------------------------------------------

@dataclass
class RunResult:
    run_id: str
    started_wall: str
    duration_s: float
    marks: list[MarkResult] = field(default_factory=list)
    valid: bool = True
    verified: bool = True
    invalid_reason: str | None = None
    integrity: dict | None = None
    frame_counts: dict = field(default_factory=dict)
    reboots: list = field(default_factory=list)
    errors: list = field(default_factory=list)
    run_dir: str = ""
    notes: list[str] = field(default_factory=list)

    def to_json(self) -> dict:
        d = asdict(self)
        d["marks"] = [{"mark": asdict(m.mark), "due_at": m.due_at,
                       "fired_at": m.fired_at, "returned_at": m.returned_at,
                       "verified": m.verified, "detail": m.detail,
                       "poll_before": {k: (m.poll_before or {}).get(k) for k in POLL_WATCH},
                       "poll_after": {k: (m.poll_after or {}).get(k) for k in POLL_WATCH}}
                      for m in self.marks]
        return d


def analyzer_present() -> bool:
    if not shutil.which("sigrok-cli"):
        return False
    try:
        r = subprocess.run(["sigrok-cli", "--scan"], capture_output=True,
                           text=True, timeout=30)
    except Exception:
        return False
    # A fx2lafw/clone device, not just the demo driver.
    return "fx2lafw" in r.stdout.lower() or "Saleae" in r.stdout


def capture(duration_s: float, out_sr: str, samplerate: str,
            channels: str, log_path: str) -> subprocess.Popen:
    """Start a blocking sigrok-cli capture as a child process.

    `out_sr` MUST be absolute: the sigrok-cli wrapper cd's into its app bundle,
    so a relative path fails with "Not a regular file". `sigrok-cli --time N`
    with a BARE integer is MILLISECONDS, so the unit suffix is mandatory.
    """
    if not os.path.isabs(out_sr):
        raise ValueError(f"capture path must be absolute: {out_sr}")
    # sigrok-cli REJECTS a float duration ("Invalid time '60.0s'"); the value
    # must be an integer count with a unit. Note also that a BARE integer is
    # milliseconds, so the unit suffix is mandatory.
    secs = int(round(duration_s))
    cmd = ["sigrok-cli", "--driver", "fx2lafw",
           "--config", f"samplerate={samplerate}",
           "--channels", channels,
           "--time", f"{secs}s",
           "--output-file", out_sr]
    with open(log_path, "a") as lf:
        lf.write("$ " + " ".join(cmd) + "\n")
    return subprocess.Popen(cmd, stdout=open(log_path, "a"),
                            stderr=subprocess.STDOUT)


def capture_ok(sr: str, duration_s: float, samplerate: str) -> tuple[bool, str]:
    """Verify the capture ran for its full duration.

    A short capture means the child died or the device detached — and a short
    capture would silently look like 'the action caused nothing'.
    """
    if not os.path.exists(sr):
        return False, "capture file missing"
    try:
        r = subprocess.run(["sigrok-cli", "-i", os.path.abspath(sr), "--show"],
                           capture_output=True, text=True, timeout=120)
    except Exception as exc:
        return False, f"could not read capture back: {exc}"
    out = r.stdout
    want = None
    mult = {"k": 1e3, "m": 1e6, "g": 1e9}
    if samplerate[-1:].lower() in mult:
        want = float(samplerate[:-1]) * mult[samplerate[-1].lower()]
    elif samplerate.isdigit():
        want = float(samplerate)
    got = None
    import re
    m = re.search(r"Logic sample count:\s*(\d+)", out)
    if m:
        got = int(m.group(1))
    if want and got:
        expect = want * duration_s
        if got < expect * 0.95:
            return False, (f"capture short: {got} samples, expected ~{int(expect)} "
                           f"({got / want:.1f}s of {duration_s}s)")
    return True, f"{got} samples" if got else "sample count not reported"


# --------------------------------------------------------------------------
# one run
# --------------------------------------------------------------------------

def run_once(run_id: str, duration_s: float, marks: list[Mark], firer: Firer,
             outdir: str, host: str, samplerate: str, channels: str,
             poll_interval: float, keep_sr: bool, max_consecutive: int = 2,
             allow_no_poll: bool = False) -> RunResult:
    started = datetime.now(timezone.utc)
    # The run directory MUST be unique per run. It was `run-<run_id>`, and a
    # manual run's id is the constant "manual" — so every manual run wrote to
    # the same directory and OVERWROTE the previous capture. Caught when a
    # second manual capture was about to clobber the frames holding the first
    # `0xE1` observation. In a sweep every run would have destroyed the last.
    run_dir = os.path.join(
        outdir, f"run-{run_id}-{started.strftime('%Y%m%d-%H%M%S')}")
    os.makedirs(run_dir, exist_ok=True)
    res = RunResult(run_id=run_id, started_wall=started.isoformat(),
                    duration_s=duration_s, run_dir=run_dir)

    sr = os.path.join(run_dir, "run.sr")
    log_path = os.path.join(run_dir, "capture.log")

    print(f"\n{'=' * 66}")
    print(f"RUN {run_id}   {duration_s:g}s   marks: "
          f"{', '.join(f'{m.at_s:g}:{m.label}' for m in marks) or '(none)'}")
    print(f"{'=' * 66}")

    # --- pre-flight -------------------------------------------------------
    p0 = poll(host)
    if p0 is None:
        if not allow_no_poll:
            res.valid = False
            res.invalid_reason = "ANSWER_KEY_UNAVAILABLE"
            res.notes.append("/poll unreachable — without the read path the harness "
                             "cannot validate anything")
            print("  ABORT: /poll unreachable")
            return res
        res.notes.append("/poll unreachable — recording anyway (--allow-no-poll); "
                         "no read-path verification and no reboot detection")
        print("  ⚠️  /poll unreachable — recording anyway")

    # --- capture + interleaved marks -------------------------------------
    poll_path = os.path.join(run_dir, "polls.jsonl")
    # TRUNCATE, never append: a stale poll from a previous attempt would mix
    # into this run's series and could fabricate a reboot (or hide one).
    open(poll_path, "w").close()

    # Retry the capture start. The analyzer re-enumerates on USB occasionally
    # (observed once mid-session), and sigrok reports "No devices found" —
    # which killed a live power-cycle capture that the operator had already
    # performed. A momentary USB drop is not a reason to abandon a run.
    proc = None
    last_err = "unknown"
    for attempt in range(1, 6):
        if not analyzer_present():
            time.sleep(3)
            last_err = "analyzer not detected on the USB bus"
            continue
        proc = capture(duration_s, sr, samplerate, channels, log_path)
        time.sleep(2.0)
        if proc.poll() is None:
            break                                   # started cleanly
        try:
            with open(log_path) as fh:
                last_err = " | ".join(fh.read().strip().splitlines()[-3:])
        except Exception:
            last_err = "capture log unreadable"
        proc = None
        print(f"  capture start failed (attempt {attempt}/5): {last_err}")
        time.sleep(3)

    if proc is None:
        res.valid = False
        res.invalid_reason = "CAPTURE_START_FAILED"
        res.notes.append(
            f"sigrok-cli could not start after 5 attempts: {last_err}")
        print(f"  ABORT: capture did not start — {last_err}")
        return res

    poller = Poller(host, poll_interval, poll_path)
    t0 = time.monotonic()
    poller.start()

    try:
        for mark in marks:
            wait = mark.at_s - (time.monotonic() - t0)
            if wait > 0:
                time.sleep(wait)
            if proc.poll() is not None:
                res.notes.append("capture process exited before all marks fired")
                break
            mr = MarkResult(mark=mark, poll_before=poller.at(time.monotonic() - t0))
            # The mark came due at its scheduled offset. In manual mode the
            # operator blocks on Enter, so the moment they pressed it is only
            # known to lie between `due_at` and the return. Attribute the mark
            # to `due_at` — it is the conservative choice, and because the diff
            # is content-based (first frame differing from baseline) the exact
            # instant only has to be approximately right.
            mr.due_at = mark.at_s
            ok, detail = firer.fire(mark)
            mr.returned_at = time.monotonic() - t0
            mr.fired_at = mr.due_at if firer.name in ("manual", "external") else mr.returned_at
            mr.fired_wall = datetime.now(timezone.utc).isoformat()
            mr.detail = detail
            # Confirm on the READ PATH, not from the HTTP response. A webhook
            # returning 200 is not evidence the fireplace acted.
            if mark.expect:
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    time.sleep(poll_interval)
                    cur = poll(host)
                    if cur and all(cur.get(k) == v for k, v in mark.expect.items()):
                        mr.verified = True
                        break
                    if proc.poll() is not None:
                        break
                else:
                    mr.verified = False
            mr.poll_after = poller.at(time.monotonic() - t0)
            res.marks.append(mr)
            flag = "" if mr.verified is not False else "  [UNCONFIRMED]"
            print(f"  mark {mark.label} due +{mr.due_at:.1f}s"
                  f"{', returned +%.1fs' % mr.returned_at if mr.returned_at else ''}"
                  f" ({detail}){flag}", flush=True)

        remaining = duration_s + 12 - (time.monotonic() - t0)
        if remaining > 0:
            try:
                proc.wait(timeout=remaining)
            except subprocess.TimeoutExpired:
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                res.notes.append("capture process had to be terminated")
    finally:
        poller.stop()
        poller.join(timeout=5)
        if proc.poll() is None:
            proc.terminate()

    # --- verify the capture itself ---------------------------------------
    ok, detail = capture_ok(sr, duration_s, samplerate)
    if not ok:
        res.valid = False
        res.invalid_reason = "CAPTURE_SHORT"
        res.notes.append(detail)

    # --- reboot / error gates --------------------------------------------
    res.reboots = poller.reboots()
    if res.reboots:
        res.valid = False
        res.invalid_reason = "REBOOT_CONTAMINATED"
        res.notes.append(f"module rebooted during the run: {res.reboots}")

    errs = set()
    for _, d in poller.samples:
        for e in (d.get("errors") or []):
            errs.add(json.dumps(e, sort_keys=True))
    res.errors = sorted(errs)
    if res.errors:
        res.notes.append(f"appliance reported errors: {res.errors}")

    # --- frames ----------------------------------------------------------
    frames = []
    for ch in [c.strip() for c in channels.split(",") if c.strip()]:
        try:
            f = decode_channel(sr, ch)
        except Exception as exc:
            res.notes.append(f"{ch}: decode failed: {exc}")
            continue
        frames.extend(f)
        res.frame_counts[ch] = len(f)
        with open(os.path.join(run_dir, f"frames_{ch}.jsonl"), "w") as fh:
            for fr in f:
                fh.write(json.dumps(fr.to_json()) + "\n")

    if not frames:
        res.valid = False
        res.invalid_reason = res.invalid_reason or "NO_FRAMES"
        res.notes.append("no CRC-valid frames decoded")
    else:
        tab = PairTable(frames)
        rep = tab.integrity()
        res.integrity = rep
        # A seq discontinuity is a WARNING, not a disqualification.
        #
        # `seq` is exchange metadata; it is not part of the payload. A break
        # means frames may have been lost or the module's counter reset — both
        # worth flagging — but the payload deltas that attribution runs on are
        # still sound. Invalidating the whole run for a break discarded the
        # genuine 0xE4 flame-on evidence on the first real ignition, because
        # the module stalled 2 s AFTER the transition.
        if rep.get("seq_advances_one_per_pair") is False:
            sa = rep.get("seq_advance", {})
            res.notes.append(
                f"SEQ ANOMALY (not fatal): counter did not advance +1 "
                f"(D0 off-by {sa.get('d0_offby')}/{sa.get('d0_steps')}, "
                f"D1 off-by {sa.get('d1_offby')}/{sa.get('d1_steps')}) — "
                f"frames may have been lost or the module counter reset")
        # A long gap in the /poll series means the module stopped answering —
        # itself a finding, and it silently removes the reboot detector.
        if poller.samples:
            gaps = [(poller.samples[i][0], poller.samples[i + 1][0])
                    for i in range(len(poller.samples) - 1)]
            worst = max((b - a for a, b in gaps), default=0)
            expected = poll_interval * 3
            if worst > expected:
                res.notes.append(
                    f"/poll GAP: longest interval between successful polls was "
                    f"{worst:.1f}s (expected ~{poll_interval:.1f}s) — the module "
                    f"stopped answering; reboot detection is blind after that point")
            if poller.samples[-1][0] < duration_s - expected:
                res.notes.append(
                    f"/poll stopped at t={poller.samples[-1][0]:.1f}s of "
                    f"{duration_s:.0f}s — no read path for the remainder")

    # --- flags ------------------------------------------------------------
    if any(m.verified is False for m in res.marks):
        res.verified = False
        res.notes.append("at least one mark was not confirmed on the read path")

    with open(os.path.join(run_dir, "run.json"), "w") as fh:
        json.dump(res.to_json(), fh, indent=2)

    if not keep_sr and res.valid:
        for f in ("run.sr",):
            p = os.path.join(run_dir, f)
            if os.path.exists(p):
                os.remove(p)

    status = "VALID" if res.valid else f"INVALID ({res.invalid_reason})"
    print(f"  -> {status}; frames={res.frame_counts or 'none'}"
          f"{'' if res.verified else '; UNVERIFIED'}")
    for n in res.notes:
        print(f"     note: {n}")
    return res


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("LISTEN-ONLY", 1)[1] if "LISTEN-ONLY" in __doc__ else "")
    ap.add_argument("--duration", type=float, default=40.0)
    ap.add_argument("--marks", default="", help="e.g. 12:flame_on,30:flame_off")
    ap.add_argument("--matrix", help="run every entry of a sweep matrix JSON")
    ap.add_argument("--mark-mode", choices=("manual", "noop", "webhook", "external"),
                    default="manual")
    ap.add_argument("--webhook-url", default=os.environ.get("FIREPLACE_WEBHOOK_URL", ""))
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--samplerate", default=DEFAULT_SAMPLERATE)
    ap.add_argument("--channels", default=DEFAULT_CHANNELS)
    ap.add_argument("--poll-interval", type=float, default=DEFAULT_POLL_S)
    ap.add_argument("--outdir", default=os.path.join(HERE, "captures"))
    ap.add_argument("--keep-sr", action="store_true",
                    help="keep the .sr files (~80 MB/run at 1 MHz)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan and exit; never opens the device")
    ap.add_argument("--max-ignitions", type=int, default=6)
    ap.add_argument("--max-runtime", type=float, default=3600)
    ap.add_argument("--max-consecutive-failures", type=int, default=2)
    ap.add_argument("--yes", action="store_true", help="skip the start confirmation")
    ap.add_argument("--allow-no-poll", action="store_true",
                    help="record even if /poll is unreachable. For captures whose "
                         "purpose does not need the read path (e.g. an ECM power-up, "
                         "where the module's HTTP server may be down while the serial "
                         "link is fine). Marks needing verification become unverified "
                         "and reboot detection is disabled.")
    args = ap.parse_args()

    marks = parse_marks(args.marks) if args.marks else []

    plan: list[tuple[str, float, list[Mark], bool]] = []
    if args.matrix:
        with open(args.matrix) as fh:
            mx = json.load(fh)
        defaults = mx.get("defaults", {})
        d_dur = float(defaults.get("capture_s",
                                   defaults.get("duration_s", args.duration)))
        runs = mx.get("runs", [])
        # `execution_order` is authoritative where present: the sweep matrix
        # deliberately runs pilot LAST (the pilot is needed for ignition) and
        # reboot-window late, which differs from the array's numeric order.
        runs = sorted(runs, key=lambda r: r.get("execution_order",
                                                r.get("run_index", 0)))
        for r in runs:
            rms = []
            for m in r.get("marks", []):
                # Accept both key spellings, and convert the matrix's
                # {field, from, to} transition form into the flat
                # {field: expected_value} the read-path gate checks.
                expect = m.get("expect")
                if not expect and isinstance(m.get("expected_poll"), dict):
                    ep = m["expected_poll"]
                    if ep.get("field") is not None and ep.get("to") is not None:
                        expect = {ep["field"]: ep["to"]}
                rms.append(Mark(
                    float(m.get("offset_s", m.get("at_s", 0))),
                    m["action"],
                    m.get("value"),
                    expect,
                    m.get("note", ""),
                ))
            reps = int(r.get("repetitions", 1))
            dur = float(r.get("capture_s", r.get("duration_s", d_dur)))
            discard = bool(r.get("discard", False))
            for i in range(reps):
                plan.append((f"{r['id']}-{i + 1}", dur, rms, discard))
    else:
        plan = [("manual", args.duration, marks, False)]

    # An IGNITION is a main-burner light-off (flame_on). Lighting the standing
    # pilot is not: it is a small diffusion flame, normally left on, and on
    # this appliance the pilot must be lit BEFORE the burner can ignite. The
    # two carry materially different risk and are counted separately.
    ignitions = sum(1 for _, _, ms, _ in plan for m in ms if m.action == "flame_on")
    pilots = sum(1 for _, _, ms, _ in plan for m in ms if m.action in ("pilot_on", "pilot_off"))

    print("=" * 66)
    print("FIREPLACE ECM LINK — COMMAND MAPPING HARNESS")
    print("=" * 66)
    for rid, dur, ms, disc in plan:
        print(f"  {rid:<22} {dur:6.0f}s  "
              f"{', '.join(f'{m.at_s:g}:{m.label}' for m in ms) or '(no marks)'}"
              f"{'   [discard]' if disc else ''}")
    print(f"\n  runs={len(plan)}  main-burner ignitions={ignitions}  "
          f"pilot ops={pilots}  mode={args.mark_mode}")
    if ignitions > args.max_ignitions:
        print(f"  ⚠️  plan needs {ignitions} ignitions but --max-ignitions is "
              f"{args.max_ignitions}; the sweep will stop early")
    print(f"  outdir={args.outdir}")
    print("\n  LISTEN-ONLY. Never clip pin 3 (+5 V). RF remote batteries OUT.")
    print("  IFT-REM means the fireplace CAN IGNITE.")
    if args.dry_run:
        print("\n--dry-run: nothing captured, nothing fired.")
        return 0
    if not args.yes:
        try:
            input("\nPress Enter to begin (Ctrl-C to abort)... ")
        except EOFError:
            print("no tty — pass --yes to run non-interactively")
            return 2

    # --- pre-flight gates -------------------------------------------------
    if not shutil.which("sigrok-cli"):
        print("ABORT: sigrok-cli not found (~/.local/bin/sigrok-cli)")
        return 2
    if not analyzer_present():
        print("ABORT: no fx2lafw logic analyzer detected — is it plugged in?")
        print("       (`sigrok-cli --scan` currently finds no capture device)")
        return 2
    if poll_with_retry(args.host) is None:
        if args.allow_no_poll:
            print(f"⚠️  /poll unreachable at {args.host} — continuing anyway "
                  f"(--allow-no-poll). Marks that need read-path verification "
                  f"will be recorded as unverified, and reboot detection is off.")
        else:
            print(f"ABORT: /poll unreachable at {args.host} after several attempts — "
                  f"the read path is the answer key, and without it nothing can be "
                  f"validated. Pass --allow-no-poll to record without it.")
            return 2

    if args.mark_mode == "noop":
        firer: Firer = NoopFirer()
    elif args.mark_mode == "webhook":
        if not args.webhook_url:
            print("ABORT: --mark-mode webhook needs --webhook-url "
                  "(or $FIREPLACE_WEBHOOK_URL)")
            return 2
        firer = WebhookFirer(args.webhook_url, host=args.host)
    elif args.mark_mode == "external":
        firer = ExternalFirer()
        print("\nEXTERNAL MODE: this process fires nothing. Fire each mark through")
        print("  Home Assistant yourself the moment it prints 'MARK DUE NOW'.")
        print("  Run this in the background so you can work in parallel.")
    else:
        firer = ManualFirer()

    os.makedirs(args.outdir, exist_ok=True)
    sweep_id = datetime.now().strftime("%Y%m%d-%H%M%S")
    index_path = os.path.join(args.outdir, f"sweep-{sweep_id}-index.jsonl")
    t_start = time.monotonic()
    consecutive_failures = 0
    ignitions_done = 0
    results: list[RunResult] = []

    def hard_stop(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, hard_stop)

    try:
        for rid, dur, ms, discard in plan:
            if time.monotonic() - t_start > args.max_runtime:
                print(f"\nHARD CAP: max runtime {args.max_runtime:g}s reached — stopping")
                break
            if ignitions_done >= args.max_ignitions:
                print(f"\nHARD CAP: max ignitions {args.max_ignitions} reached — stopping")
                break

            r = run_once(rid, dur, ms, firer, args.outdir, args.host,
                         args.samplerate, args.channels, args.poll_interval,
                         args.keep_sr, allow_no_poll=args.allow_no_poll)
            results.append(r)
            # Discarded runs calibrate the reconnect/settle window; they are
            # kept on disk for inspection but must never enter attribution.
            with open(index_path, "a") as fh:
                d = r.to_json()
                d["discard"] = discard
                fh.write(json.dumps(d) + "\n")

            ignitions_done += sum(1 for m in ms
                                  if m.action == "flame_on" and not r.errors)
            consecutive_failures = (0 if r.valid else consecutive_failures + 1) if not discard \
                else consecutive_failures
            if r.errors:
                print(f"\nABORT: appliance reported errors: {r.errors}")
                break
            if consecutive_failures >= args.max_consecutive_failures:
                print(f"\nABORT: {consecutive_failures} consecutive invalid runs")
                break
    except KeyboardInterrupt:
        print("\n\nInterrupted — restoring state.")
    finally:
        # Safety path: always try to leave the appliance off and the safety
        # timer disarmed, and always write what we have. A partial result with
        # explicit gaps beats a runaway sweep on a gas appliance.
        print("\n--- restoring state (best effort) ---")
        try:
            firer.reset()
        except Exception as exc:
            print(f"  reset failed: {exc}")
        valid = sum(1 for r in results if r.valid)
        print(f"\n{len(results)} run(s), {valid} valid")
        print(f"index: {index_path}")
        print("NEXT: python3 tools/ecm_diff.py " + index_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
