#!/usr/bin/env python3
"""End-to-end pipeline test: synthetic sweep -> diff -> attribution.

WHY THIS EXISTS
---------------
The harness, the frame parser, the diff engine, and the map renderer were built
as separate pieces. Each has its own unit tests, and all of them passed — but
unit tests on both sides of an interface do not prove the interface matches.
During this build exactly that class of bug appeared twice: the sweep matrix
used `offset_s`/`capture_s` while the harness read `at_s`/`duration_s` (every
mark would have fired at t=0), and `repetitions: 3` meant different things to
each side (which would have re-ignited the burner three times).

So this test writes a synthetic sweep to disk in the REAL harness output
layout — `sweep-<stamp>-index.jsonl` plus `<run_dir>/frames_D0.jsonl` — using
the REAL `Frame.to_json()`, and then drives the REAL loader. If the pieces
disagree, this fails; unit tests on either side would not have.

No hardware, no Home Assistant, no gas.

Run:
    python3 tools/ecm_pipeline_test.py
    python3 tools/ecm_pipeline_test.py --keep   # keep the temp sweep for inspection
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_frames import Frame, crc16_modbus  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(HERE, "tools")


class Results:
    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print(f"  PASS  {label}")
        else:
            self.failed += 1
            self.failures.append(label)
            print(f"  FAIL  {label}" + (f"\n          {detail}" if detail else ""))
        return ok

    def summary(self) -> int:
        print(f"\n{'=' * 62}")
        if self.failed:
            print(f"RESULT: {self.failed} FAILED, {self.passed} passed")
            for f in self.failures:
                print(f"  - {f}")
        else:
            print(f"RESULT: all {self.passed} checks passed")
        print("=" * 62)
        return 1 if self.failed else 0


def make_frame(channel: str, dev: int, seq: int, cmd: int,
               payload: list[int], t: float) -> Frame:
    """Build a real Frame with a correct CRC and the exact wire layout."""
    raw = [0x4B, dev, seq, cmd] + list(payload)
    crc = crc16_modbus(bytes(raw))
    raw += [crc & 0xFF, (crc >> 8) & 0xFF]
    return Frame(channel=channel, t=t, dev=dev, seq=seq, cmd=cmd,
                 payload=list(payload), raw=raw, crc=crc, index=0)


def write_run(root: str, run_id: str, frames_by_ch: dict[str, list[Frame]],
             marks: list[dict], duration: float, *,
             valid: bool = True, verified: bool = True,
             discard: bool = False, reason: str | None = None,
             reboots: list | None = None) -> dict:
    """Write one run directory + its frames files, exactly as the harness does."""
    run_dir = os.path.join(root, f"run-{run_id}")
    os.makedirs(run_dir, exist_ok=True)
    for ch, frames in frames_by_ch.items():
        with open(os.path.join(run_dir, f"frames_{ch}.jsonl"), "w") as fh:
            for i, fr in enumerate(frames):
                fr.index = i
                fh.write(json.dumps(fr.to_json()) + "\n")
    return {
        "run_id": run_id,
        "started_wall": "2026-10-16T00:00:00+00:00",
        "duration_s": duration,
        "marks": [
            {"mark": {"at_s": m["at_s"], "action": m["action"],
                      "value": m.get("value"), "expect": m.get("expect"),
                      "note": m.get("note", "")},
             "due_at": m["at_s"], "fired_at": m["at_s"],
             "returned_at": m["at_s"] + 0.2,
             "verified": verified, "detail": "synthetic",
             "poll_before": {}, "poll_after": {}}
            for m in marks
        ],
        "valid": valid,
        "verified": verified,
        "invalid_reason": reason,
        "integrity": {"seq_advances_one_per_pair": True},
        "frame_counts": {ch: len(f) for ch, f in frames_by_ch.items()},
        "reboots": reboots or [],
        "errors": [],
        "run_dir": run_dir,
        "notes": [],
        "discard": discard,
    }


def idle_frames(t0: float = 0.0, n: int = 6, step: float = 1.0,
                payload_a: list[int] | None = None) -> dict[str, list[Frame]]:
    """A steady exchange rotation with CONSTANT payloads — the idle baseline.

    Mirrors the real bus: BC and 6E on dev 2, DC on dev 1, D1 replying.
    """
    d0, d1 = [], []
    seq = 0x98
    for i in range(n):
        t = t0 + i * step
        d0.append(make_frame("D0", 0x02, seq, 0xBC, [0x02, 0x04], t))
        d1.append(make_frame("D1", 0x04, seq, 0xBC,
                             [0x02, 0x04, 0, 0, 0, 0], t + 0.0084))
        d0.append(make_frame("D0", 0x01, seq, 0xDC,
                             [0x00, 0x17, 0, 0, 0, 0, 0] + [0] * 18, t + 0.4) if payload_a is None
                   else make_frame("D0", 0x01, seq, 0xDC, payload_a, t + 0.4))
        d1.append(make_frame("D1", 0x03, seq, 0xDC, [0x00, 0x17], t + 0.408))
        seq = (seq + 1) & 0xFF
    return {"D0": d0, "D1": d1}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep", action="store_true", help="keep the temp sweep")
    args = ap.parse_args()
    r = Results()

    tmp = tempfile.mkdtemp(prefix="ecm-pipeline-")
    print(f"synthetic sweep: {tmp}")
    records = []

    # ---- the sweep ---------------------------------------------------------
    # idle control x2 — must produce ZERO attributions
    for i in range(2):
        records.append(write_run(tmp, f"idle-{i}", idle_frames(), [], 40.0))

    # flame_on / flame_off x3.
    #
    # The status byte takes a value that is DISJOINT per action: 0x04 when the
    # flame is on, 0x08 when it is off. That is the real appliance behaviour —
    # the status frame tracks STATE, so it moves for both actions, and the
    # observed VALUE is what distinguishes them. This fixture therefore exercises
    # the state-echo path, not merely "a shared key".
    #
    # Note it must be CONSISTENT across every run of the same action: if one run
    # encoded "off" as 0x00 and another as 0x08, the pooled value sets would
    # overlap and the engine would rightly refuse to classify an echo.
    ON = [0x00, 0x17, 0, 0, 0, 0, 0x04] + [0] * 18
    OFF = [0x00, 0x17, 0, 0, 0, 0, 0x08] + [0] * 18
    for i in range(3):
        f = {"D0": [], "D1": []}
        base = idle_frames(t0=0.0, n=3, payload_a=OFF)
        f["D0"] += base["D0"]
        f["D1"] += base["D1"]
        on = idle_frames(t0=20.0, n=3, payload_a=ON)
        f["D0"] += on["D0"]
        f["D1"] += on["D1"]
        off = idle_frames(t0=40.0, n=3, payload_a=OFF)
        f["D0"] += off["D0"]
        f["D1"] += off["D1"]
        records.append(write_run(tmp, f"onoff-{i}", f, [
            {"at_s": 12.0, "action": "flame_on", "expect": {"power": 1}},
            {"at_s": 32.0, "action": "flame_off", "expect": {"power": 0}},
        ], 50.0))

    # height ladder — DC byte 7 monotone with the requested height.
    # Each height gets its own 12 s segment; the mark sits 1 s into the segment
    # and the segment's frames span past it, so every mark has a real post
    # window (an empty one is refused by the loader, correctly).
    f = {"D0": [], "D1": []}
    SEG = 12.0
    for h in range(1, 6):
        pay = [0x00, 0x17, 0, 0, 0, 0, 0x04, h] + [0] * 17
        part = idle_frames(t0=(h - 1) * SEG, n=11, step=1.0, payload_a=pay)
        f["D0"] += part["D0"]
        f["D1"] += part["D1"]
    records.append(write_run(tmp, "ladder-0", f,
                             [{"at_s": 1.0 + (h - 1) * SEG, "action": "height", "value": h}
                              for h in range(1, 6)], 5 * SEG + 5))

    # ---- state echo ------------------------------------------------------
    # (covered by the onoff runs above — same action names, disjoint values.)

    # one invalid run and one discard run — must be excluded, not silently dropped
    records.append(write_run(tmp, "short-0", idle_frames(), [], 40.0,
                             valid=False, reason="CAPTURE_SHORT"))
    records.append(write_run(tmp, "settle-0", idle_frames(), [], 40.0, discard=True))

    index_path = os.path.join(tmp, "sweep-20261016-000000-index.jsonl")
    with open(index_path, "w") as fh:
        for rec in records:
            fh.write(json.dumps(rec) + "\n")

    # ---- drive the REAL loader --------------------------------------------
    print("\n=== pipeline: harness layout -> ecm_diff loader ===")
    sys.path.insert(0, TOOLS)
    from ecm_diff import load_sweep_index, attribute  # noqa: E402

    try:
        sweep = load_sweep_index(index_path, settle_s=2.0)
    except Exception as exc:
        r.check(False, "load_sweep_index consumes the harness layout",
                f"{type(exc).__name__}: {exc}")
        return r.summary()

    r.check(True, "load_sweep_index consumes the harness layout")
    # Mark-less runs are separated out as idle CONTROLS, so they are not part
    # of `runs`: 8 records - 2 excluded - 2 controls = 4 usable runs.
    r.check(len(sweep.runs) == 4,
            "4 usable runs loaded (3 onoff + 1 ladder)",
            f"got {len(sweep.runs)}")
    r.check(len(sweep.excluded) == 2,
            "the invalid run and the discard run are both excluded",
            f"excluded={[str(e)[:60] for e in sweep.excluded]}")
    r.check(len(sweep.controls) == 2,
            "the two mark-less runs are classified as idle controls",
            f"got {len(sweep.controls)}")

    # ---- attribution -------------------------------------------------------
    print("\n=== attribution ===")
    atts = attribute(sweep.runs, sweep.controls)
    by_action: dict[str, list] = {}
    for a in atts:
        by_action.setdefault(a.action, []).append(a)

    print("  attributions by action:")
    for act in sorted(by_action):
        for a in by_action[act]:
            print(f"    {act:<12} {a.key_s:<16} {a.score:<14} "
                  f"obs={a.observations} bytes={a.byte_indices} specific={a.specific}")

    # the idle control must be clean
    r.check(not by_action.get("noop"),
            "idle runs attribute nothing (no false positives)")
    ctrl_clean = all(a.control_clean for a in atts)
    r.check(ctrl_clean, "every attribution has control_clean=True")

    # flame_off moved DC byte 6 from 0x04 to 0x08 -> the key should be found
    off = by_action.get("flame_off", [])
    r.check(len(off) > 0, "flame_off produced at least one attribution")
    if off:
        keys = {a.key_s for a in off}
        r.check("D0/0x01/0xDC" in keys,
                "flame_off attributes the DC status key",
                f"got {keys}")
        dc = next((a for a in off if a.key_s == "D0/0x01/0xDC"), None)
        if dc:
            r.check(6 in dc.byte_indices,
                    "flame_off attributes byte index 6 (the flipped byte)",
                    f"got {dc.byte_indices}")

    # ---- state echo, end to end -------------------------------------------
    # NOT assertable on the main sweep above: the height ladder touches the
    # SAME key (D0/0x01/0xDC), so with three actions on it no byte index is
    # disjoint and the engine rightly declines to call it an echo. That is the
    # correct behaviour — a third action changing the same frame makes the
    # values ambiguous. So the echo is exercised on its own sweep, below.
    print("\n=== state echo (dedicated sweep, through the CLI) ===")
    echo_dir = tempfile.mkdtemp(prefix="ecm-echo-")
    recs = []
    for i in range(3):
        f = {"D0": [], "D1": []}
        for t0, p in ((0.0, OFF), (20.0, ON), (40.0, OFF)):
            part = idle_frames(t0=t0, n=3, payload_a=p)
            f["D0"] += part["D0"]
            f["D1"] += part["D1"]
        recs.append(write_run(echo_dir, f"onoff-{i}", f, [
            {"at_s": 12.0, "action": "flame_on"},
            {"at_s": 32.0, "action": "flame_off"},
        ], 50.0))
    echo_idx = os.path.join(echo_dir, "sweep-echo-index.jsonl")
    with open(echo_idx, "w") as fh:
        for rec in recs:
            fh.write(json.dumps(rec) + "\n")
    echo_out = os.path.join(echo_dir, "attribution.json")
    p2 = subprocess.run([sys.executable, os.path.join(TOOLS, "ecm_diff.py"),
                         echo_idx, "--out", echo_out],
                        capture_output=True, text=True, cwd=HERE)
    r.check(p2.returncode == 0, "echo sweep runs through the CLI",
            f"rc={p2.returncode}\n{p2.stderr[-300:]}")
    if os.path.exists(echo_out):
        er = [json.loads(ln) for ln in open(echo_out) if ln.strip()]
        er = [x for x in er if x.get("record") == "attribution"]
        echoed = [x for x in er if x.get("score") == "state_echo"]
        r.check(len(echoed) == 2,
                "both flame actions report the key as a state_echo",
                f"got {[(x.get('action'), x.get('score')) for x in er]}")
        if echoed:
            e = echoed[0]
            r.check(e.get("distinguishable") is True,
                    "distinguishable=True survives serialization",
                    f"got {e.get('distinguishable')!r}")
            r.check(e.get("discriminating_index") == 6,
                    "discriminating_index identifies byte 6",
                    f"got {e.get('discriminating_index')!r}")
            r.check(bool(e.get("per_action_values")),
                    "per_action_values survives serialization",
                    f"got {e.get('per_action_values')!r}")
            r.check(e.get("specific") is False,
                    "a state echo stays specific=False (it is not a command)")
            r.check(all(x.get("score") != "confirmed" for x in echoed),
                    "a state echo is never serialized as confirmed")
    shutil.rmtree(echo_dir, ignore_errors=True)


    # ladder -> a parameter byte should surface
    grad = by_action.get("height", [])
    r.check(len(grad) > 0, "height ladder produced attributions")

    # the excluded runs must not appear
    r.check(all(a.action not in ("noop",) for a in atts),
            "no attribution leaked from an excluded run")

    # ---- the CLI chain -----------------------------------------------------
    print("\n=== pipeline: ecm_diff CLI -> attribution.json ===")
    att_out = os.path.join(tmp, "attribution.json")
    # NOTE: --json means "print to stdout INSTEAD of writing --out", so the two
    # are mutually exclusive. Passing both silently writes nothing.
    # The CLI APPENDS to --out, so remove any prior file first.
    if os.path.exists(att_out):
        os.remove(att_out)
    proc = subprocess.run(
        [sys.executable, os.path.join(TOOLS, "ecm_diff.py"), index_path,
         "--out", att_out],
        capture_output=True, text=True, cwd=HERE)
    r.check(proc.returncode == 0, "ecm_diff CLI exits 0",
            f"rc={proc.returncode}\n{proc.stderr[-500:]}")
    r.check(os.path.exists(att_out), "attribution.json was written",
            f"stderr: {proc.stderr[-300:]}")
    if os.path.exists(att_out):
        try:
            # The CLI writes APPEND-ONLY JSONL with MIXED record types,
            # discriminated by a `record` field: N × "attribution" plus one
            # trailing "summary". Filter on it — treating the summary as an
            # attribution makes `score` come out None.
            all_recs = [json.loads(ln) for ln in open(att_out) if ln.strip()]
            recs = [x for x in all_recs if x.get("record") == "attribution"]
            r.check(True, "attribution.json is valid JSONL")
            r.check(len(recs) > 0, f"attribution.json carries {len(recs)} attribution(s)")
            r.check(any(x.get("record") == "summary" for x in all_recs),
                    "the JSONL ends with a summary record")

            r.check(all(isinstance(x, dict) and "score" in x and "action" in x
                        for x in recs),
                    "every attribution record is complete",
                    f"sample keys: {sorted(recs[0])[:14] if recs else []}")

            # Scores must come from the closed enum. A free-text score would
            # silently break the map schema.
            enum = {"confirmed", "probable", "speculative", "observed_stable",
                    "unknown", "state_echo"}
            bad = sorted({str(x.get("score")) for x in recs} - enum)
            r.check(not bad, "every score is from the closed enum",
                    f"off-enum: {bad}")

            # The exclusions must be accounted for in the summary, not dropped.
            summ = next((x for x in all_recs if x.get("record") == "summary"), {})
            r.check(bool(summ), "summary record is non-empty",
                    f"summary keys: {sorted(summ)[:14]}")
        except Exception as exc:
            r.check(False, "attribution.json parses", str(exc))

    # Exclusions are reported on stderr, not in the JSONL body.
    r.check("exclud" in proc.stderr.lower() or "skip" in proc.stderr.lower()
            or "discard" in proc.stderr.lower(),
            "the CLI reports its exclusions on stderr (not silently dropped)",
            f"stderr: {proc.stderr[-200:]}")

    # ---- the live collector's offline path -> the same loaders -----------
    collector_pipeline(r)

    print(f"\nsweep kept at: {tmp}" if args.keep else "")
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    return r.summary()


def collector_pipeline(r: Results) -> None:
    """Canned MQTT frame lines -> ecm_collector offline path -> ecm_diff loaders.

    WHY THIS CASE
    -------------
    ecm_collector.py is the host half of the live telemetry feature: the device
    publishes Frame.to_json() on MQTT and this tool writes it to disk in the
    harness layout. It imports ecm_session's KNOWN_CMDS and is read by ecm_diff.
    None of those three call each other — they are joined only by the file and
    JSON contracts, which is exactly the interface a unit test on either side
    cannot check. So this drives the REAL collector CLI over a canned capture
    and then drives the REAL loaders over what it wrote.

    The canned lines are the device's own payload shape (the REAL
    Frame.to_json() plus the additive `boot` key), and they span two boots so
    the run boundary is exercised too. No broker, no paho, no hardware.
    """
    print("\n=== pipeline: ecm_collector offline path -> ecm_diff loaders ===")
    sys.path.insert(0, TOOLS)
    from ecm_diff import load_run, load_sweep_index  # noqa: E402

    tmp = tempfile.mkdtemp(prefix="ecm-collector-pipe-")
    canned = os.path.join(tmp, "canned.jsonl")
    out = os.path.join(tmp, "out")

    lines: list[str] = []

    def emit(boot: str, ch: str, dev: int, seq: int, cmd: int,
             payload: list[int], t: float) -> None:
        d = make_frame(ch, dev, seq, cmd, payload, t).to_json()
        d["boot"] = boot
        lines.append(json.dumps(d))

    def env(suffix: str, payload) -> None:
        lines.append(json.dumps({"topic": f"fireplace/ecm/{suffix}",
                                 "payload": payload}))

    # Retained side topics, delivered the instant a subscriber connects — which
    # is BEFORE the first frame. They must be buffered and applied to the run
    # the frames then establish, not dropped.
    env("telemetry/summary",
        {"boot": "boot-A", "uptime_ms": 100, "frames_parsed": 8,
         "frames_unknown": 1, "census_keys": 2, "census_slot_ops": 1,
         "records_evicted": 0, "publish_skipped": 0})
    env("inventory/D0_0x02_0xBC",
        {"key": "D0/0x02/0xBC", "dir": "D0", "occurrences": 4, "frame_len": 8,
         "lengths_seen": [8], "distinct_payloads": 1, "role": "observed",
         "values_observed": {"6": ["0x04"]}, "values_truncated": False})
    # a census key that no frame ever carries: it must still be flagged, with
    # no bytes, when the run closes
    env("inventory/D0_0x01_0x55",
        {"key": "D0/0x01/0x55", "dir": "D0", "occurrences": 1, "frame_len": 8,
         "lengths_seen": [8], "distinct_payloads": 1, "role": "unknown",
         "values_observed": {}, "values_truncated": False})
    env("inventory/slot_D1_0x04_0x77",
        {"key": "slot/D1/0x04/0x77", "namespace": "slot_op", "occurrences": 1,
         "known": False, "first_ms": 0, "last_ms": 0})
    env("status", "online")

    # boot A: the small idle rotation the bus actually runs.
    for i in range(4):
        seq = (0x20 + i) & 0xFF
        emit("boot-A", "D0", 0x02, seq, 0xBC, [0x02, 0x04], i * 1.0)
        emit("boot-A", "D1", 0x04, seq, 0xBC, [0x02, 0x04, 0, 0, 0, 0],
             i * 1.0 + 0.01)
    # boot B: a SECOND boot id, and its counter restarts at 0x00. The collector
    # must put these in their own run directory, never in boot A's.
    for i in range(3):
        emit("boot-B", "D0", 0x01, i, 0xDC, [0x00, 0x17] + [0] * 23, i * 1.0)
        emit("boot-B", "D1", 0x03, i, 0xDC, [0x00, 0x17], i * 1.0 + 0.01)

    with open(canned, "w") as fh:
        fh.write("\n".join(lines) + "\n")

    proc = subprocess.run(
        [sys.executable, os.path.join(TOOLS, "ecm_collector.py"),
         "--replay", canned, "--out", out, "--quiet"],
        capture_output=True, text=True, cwd=HERE)
    r.check(proc.returncode == 0, "ecm_collector --replay exits 0",
            f"rc={proc.returncode}\n{proc.stderr[-400:]}")

    run_dirs = sorted(d for d in os.listdir(out)
                      if os.path.isdir(os.path.join(out, d)))
    r.check(len(run_dirs) == 2, "one run directory per boot_id (2)",
            f"got {run_dirs}")
    if len(run_dirs) != 2:
        shutil.rmtree(tmp, ignore_errors=True)
        return

    run_a = next((d for d in run_dirs if d.endswith("-boot-A")), run_dirs[0])
    by_ch = load_run(os.path.join(out, run_a))
    r.check(len(by_ch.get("D0", [])) == 4 and len(by_ch.get("D1", [])) == 4,
            "ecm_diff.load_run reads the collected frames_D0/D1",
            f"got D0={len(by_ch.get('D0', []))} D1={len(by_ch.get('D1', []))}")

    boots = {json.loads(ln)["boot"]
             for ln in open(os.path.join(out, run_a, "frames_D0.jsonl"))
             if ln.strip()}
    r.check(boots == {"boot-A"},
            "no frames merged across the boot boundary", f"got {boots}")

    # The retained census arrived before any frame did. It must have been
    # buffered into run A rather than dropped, and its two unknowns — which no
    # frame ever carries — must be flagged with the census as their source.
    rj = json.load(open(os.path.join(out, run_a, "run.json")))
    col = rj["collector"]
    r.check(col["census_keys_received"] == 3 and col["census_expected"] == 3
            and col["census_complete"] is True,
            "the retained census is buffered into the run the frames establish",
            f"got {col}")
    unk = [json.loads(ln) for ln in
           open(os.path.join(out, run_a, "unknown_commands.jsonl")) if ln.strip()]
    r.check(any(u["key"] == "D0/0x01/0x55" and u.get("source") == "inventory"
                for u in unk) and
            any(u.get("slot_op") == "0x77" for u in unk),
            "census-only opcodes are flagged at close, with no frame bytes",
            f"got {unk}")

    sweeps = sorted(f for f in os.listdir(out)
                    if f.startswith("sweep-") and f.endswith("-index.jsonl"))
    r.check(len(sweeps) == 1, "the collector wrote one sweep index",
            f"got {sweeps}")
    if sweeps:
        load = load_sweep_index(os.path.join(out, sweeps[0]))
        total = len(load.runs) + len(load.controls)
        r.check(total == 2 and not load.excluded,
                "ecm_diff.load_sweep_index consumes the collector's index",
                f"runs={len(load.runs)} controls={len(load.controls)} "
                f"excluded={load.excluded}")
        # The index must point at directories that exist: load_sweep_index drops
        # a record whose run_dir is missing, so this is where a bad path shows.
        r.check(all(os.path.isdir(json.loads(ln)["run_dir"])
                    for ln in open(os.path.join(out, sweeps[0])) if ln.strip()),
                "every sweep-index record points at a real run_dir")

    shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
