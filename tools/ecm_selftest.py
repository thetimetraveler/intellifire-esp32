#!/usr/bin/env python3
"""Self-tests for the ECM frame parser. No hardware, no Home Assistant.

V1 (golden fixture) is the most valuable test in the project: it validates the
whole parse -> CRC -> framing -> seq -> pairing chain against known-good data,
using only the 1 MHz capture already on disk. If V1 passes, the parser is
correct and every later result can be trusted at the framing layer.

Run:
    python3 tools/ecm_selftest.py
    python3 tools/ecm_selftest.py --vcd /path/to/other.vcd --sr /path/to/dump1.sr
"""
from __future__ import annotations

import argparse
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_frames import decode_channel, PairTable, SEC_PER_BYTE  # noqa: E402

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_VCD = os.path.join(HERE, "logic_analyzer_data", "dump3.vcd")
DEFAULT_SR = os.path.join(HERE, "logic_analyzer_data", "dump1.sr")

# The eight frames of the 1 MHz capture, as raw hex. Every one is
# CRC-validated; these are the exact bytes sigrok-cli decodes.
GOLDEN: dict[str, list[str]] = {
    "D0": [
        "4B 02 98 BC 02 04 98 47",
        "4B 01 99 DC 00 17 00 00 00 00 00 04 " + " ".join(["00"] * 17) + " F5 34",
        "4B 02 9A BC 02 04 99 FF",
        "4B 02 9B 6E 00 01 F9 59",
    ],
    "D1": [
        "4B 04 98 BC 02 04 00 00 00 00 B4 D5",
        "4B 03 99 DC 00 17 E4 C8",
        "4B 04 9A BC 02 04 00 00 00 00 35 0C",
        "4B 04 9B 6E 00 01 00 99 24",
    ],
}

# Command byte -> frame length, keyed per direction. The reply direction uses
# a DIFFERENT length for the same command byte (DC is 31 bytes on D0 but 8 on
# D1), so this cannot be keyed on cmd alone.
#
# Every entry here is CONFIRMED against the 1 MHz capture. Note D1/0x6E is 9,
# not 12 — the 12-byte figure came from an unverified summary and is wrong.
GOLDEN_LEN = {
    "D0": {0xBC: 8, 0xDC: 31, 0x6E: 8},
    "D1": {0xBC: 12, 0xDC: 8, 0x6E: 9},
}


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


def v1_golden(vcd: str, r: Results) -> list:
    """V1 — reproduce the eight known frames exactly, and derive the protocol model."""
    print(f"\n=== V1: golden fixture ({os.path.basename(vcd)}) ===")
    if not os.path.exists(vcd):
        r.check(False, "fixture exists", f"missing: {vcd}")
        return []

    frames = []
    for ch in ("D0", "D1"):
        try:
            f = decode_channel(vcd, ch)
        except Exception as exc:
            r.check(False, f"{ch} decodes", str(exc))
            continue
        frames.extend(f)

        # 1. frame count
        r.check(len(f) == len(GOLDEN[ch]),
                f"{ch}: {len(GOLDEN[ch])} frames found",
                f"got {len(f)}")

        # 2. every frame byte-for-byte
        for want, got in zip(GOLDEN[ch], f):
            r.check(got.hex == want, f"{ch}: frame matches  {want[:26]}…",
                    f"want {want}\n          got  {got.hex}")

        # 3. lengths match the per-direction table
        for got in f:
            want_len = GOLDEN_LEN[ch].get(got.cmd)
            if want_len is not None:
                r.check(len(got.raw) == want_len,
                        f"{ch}: cmd 0x{got.cmd:02X} length == {want_len}",
                        f"got {len(got.raw)}")

        # 4. no ambiguity — the boundary check should have resolved every frame
        r.check(all(not g.ambiguous for g in f), f"{ch}: no ambiguous frames",
                f"{[g.hex for g in f if g.ambiguous]}")

    if len(frames) != 8:
        return frames

    # --- derived properties: these are the protocol model, confirmed from data ---
    tab = PairTable(frames)
    rep = tab.integrity()

    r.check(rep["seq"]["shared"] == ["0x98", "0x99", "0x9A", "0x9B"],
            "seq values are 0x98..0x9B on both directions",
            str(rep["seq"]))

    # THE key correction: seq appears ONCE PER DIRECTION, not once per frame.
    # An exchange counter, not a global frame counter.
    r.check(rep["seq"]["each_shared_once_per_direction"] is True,
            "seq is an EXCHANGE counter (each value once per direction)",
            str(rep["seq"]))

    r.check(rep["dev_offset_is_2"] is True,
            "D1 device id == D0 device id + 2",
            str(rep["dev_offset_histogram"]))

    r.check(rep["seq_advances_one_per_pair"] is True,
            "seq advances +1 per exchange pair",
            str(rep["seq_pair_deltas"]))

    r.check(rep["dev_values"]["D0"] == [1, 2] and rep["dev_values"]["D1"] == [3, 4],
            "device ids are {1,2} on D0 and {3,4} on D1",
            str(rep["dev_values"]))

    # D1 replies exactly one D0-frame-duration after D0 starts -> request/response
    lat = rep["reply_latency"]
    r.check(len(lat) == 4, "reply latency measured for all 4 exchanges", str(len(lat)))
    if lat:
        ratios = [x["ratio"] for x in lat if x["ratio"]]
        r.check(all(0.99 <= x <= 1.02 for x in ratios),
                "D1 starts one D0-frame-duration after D0 (request/response)",
                f"ratios={ratios}")
    return frames


def v2_sr_smoke(sr: str, r: Results) -> None:
    """V2 — does the .sr decode path work at all?

    Deliberately weak, and asserts the right thing. dump1 is a 20 kHz capture
    (~2.08 samples/bit) whose D1 decode is corrupt, so we assert ONLY that the
    sigrok invocation succeeds and yields bytes with the expected start byte.
    Asserting frames here would be asserting something we expect to fail.

    Note the .sr path needs samplenum output to carry times; without a
    sample rate the bytes still arrive but frame boundary checks degrade.
    """
    print(f"\n=== V2: .sr decode smoke test ({os.path.basename(sr)}) ===")
    if not os.path.exists(sr):
        print(f"  SKIP  {sr} not present")
        return
    from ecm_frames import extract_bytes_with_times, sigrok_decode
    try:
        text = sigrok_decode(sr, "D0")
    except Exception as exc:
        r.check(False, "sigrok decodes the .sr without error", str(exc))
        return
    r.check(True, "sigrok decodes the .sr without error")

    raw, _ = extract_bytes_with_times(text)
    r.check(len(raw) > 0, "bytes are produced from the .sr", f"got {len(raw)}")
    r.check(raw.count(0x4B) > 0,
            "0x4B start byte appears in the .sr decode",
            f"{raw.count(0x4B)} occurrences in {len(raw)} bytes")


def v2b_absolute_path(r: Results) -> None:
    """The sigrok wrapper cd's into its bundle; a relative path must be refused."""
    print("\n=== V2b: absolute-path guard ===")
    from ecm_frames import load_channel
    try:
        load_channel("logic_analyzer_data/dump3.vcd", "D0")
        r.check(False, "relative path is rejected before reaching sigrok")
    except ValueError as exc:
        r.check("absolute" in str(exc), "relative path is rejected before reaching sigrok",
                str(exc))
    except Exception as exc:
        r.check(False, "relative path is rejected before reaching sigrok",
                f"wrong exception: {type(exc).__name__}: {exc}")


def v2c_decoder_flag(r: Results) -> None:
    """The flag must be -P/--protocol-decoders; the singular form is invalid."""
    print("\n=== V2c: decoder flag ===")
    import subprocess
    vcd = DEFAULT_VCD
    if not os.path.exists(vcd):
        print("  SKIP  fixture not present")
        return
    bad = subprocess.run(
        ["sigrok-cli", "-i", vcd, "--protocol-decoder", "uart:baudrate=9600:rx=D0"],
        capture_output=True, text=True)
    r.check("Unknown option" in (bad.stderr + bad.stdout),
            "singular --protocol-decoder is correctly rejected")
    good = subprocess.run(
        ["sigrok-cli", "-i", vcd, "-P", "uart:baudrate=9600:rx=D0",
         "-A", "uart=rx-data"],
        capture_output=True, text=True)
    r.check("4B" in good.stdout, "-P/--protocol-decoders works")


def v1b_seq_wrap(r: Results) -> None:
    """The seq counter WRAPS at 256, and the integrity check must survive it.

    Regression test for a real false positive: the check originally diffed the
    SORTED SET of distinct seq values, so a capture long enough to cross the
    wrap produced one huge bogus delta and a false SEQ_BROKEN verdict — on the
    first live 60 s capture, whose real sequence was a perfect 160/160 of +1.
    """
    print("\n=== V1b: seq wraparound must not read as broken ===")
    from ecm_frames import Frame, PairTable
    import importlib
    ef = importlib.import_module("ecm_frames")

    # Build frames locally so this test never depends on a private helper.
    def frame(ch: str, dev: int, seq: int, t: float) -> Frame:
        raw = [0x4B, dev, seq, 0xBC, 0x02, 0x04]
        crc = ef.crc16_modbus(bytes(raw))
        raw += [crc & 0xFF, (crc >> 8) & 0xFF]
        return Frame(channel=ch, t=t, dev=dev, seq=seq, cmd=0xBC,
                     payload=[0x02, 0x04], raw=raw, crc=crc)

    # seq runs 0xFA -> 0x00 -> 0x09, i.e. it crosses the wrap.
    seqs = list(range(0xFA, 0x100)) + list(range(0x00, 0x0A))
    d0 = [frame("D0", 0x02, s, i * 0.2) for i, s in enumerate(seqs)]
    d1 = [frame("D1", 0x04, s, i * 0.2 + 0.008) for i, s in enumerate(seqs)]
    rep = PairTable(d0 + d1).integrity()
    r.check(rep.get("seq_advances_one_per_pair") is True,
            "a wrap-crossing sequence is reported as advancing +1",
            f"got {rep.get('seq_advances_one_per_pair')!r} "
            f"offby d0={rep.get('seq_advance', {}).get('d0_offby')} "
            f"d1={rep.get('seq_advance', {}).get('d1_offby')}")
    r.check(rep.get("seq_advance", {}).get("wrap_observed") is True,
            "the wrap is detected and reported", str(rep.get("seq_advance")))

    # A genuine skip must still be caught — and it must be constructed so it
    # does NOT land on zero, because a jump to zero is indistinguishable from
    # the ECM parking its counter, which is a known benign event, not damage.
    # (The first version of this test shifted a value onto 0x00 and the
    # classifier correctly called it a park entry — the test was ambiguous,
    # not the code.)
    gseqs = [0x10, 0x11, 0x12, 0x14, 0x15, 0x16]      # 0x13 dropped
    gd0 = [frame("D0", 0x02, s, i * 0.2) for i, s in enumerate(gseqs)]
    gd1 = [frame("D1", 0x04, s, i * 0.2 + 0.008) for i, s in enumerate(gseqs)]
    rep2 = PairTable(gd0 + gd1).integrity()
    r.check(rep2.get("seq_advances_one_per_pair") is False,
            "a real skip (a dropped frame, landing non-zero) is still detected",
            f"got {rep2.get('seq_advances_one_per_pair')!r} "
            f"skipped={rep2.get('seq_advance', {}).get('d0_skipped')}")
    r.check(rep2.get("seq_advance", {}).get("d0_skipped") == 1,
            "and it is counted as exactly one SKIP",
            str(rep2.get("seq_advance")))

    # ...whereas the ECM PARKING must NOT be reported as a skip. This is the
    # behaviour that inflated a single stall into "27 off-by" anomalies.
    # Four consecutive 0x00 frames give THREE 00->00 pairs, i.e. 3 HELD steps;
    # the step into the run (0x42 -> 0x00) is the single park ENTRY.
    pseqs = [0x40, 0x41, 0x42, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02]
    pd0 = [frame("D0", 0x02, s, i * 0.2) for i, s in enumerate(pseqs)]
    pd1 = [frame("D1", 0x04, s, i * 0.2 + 0.008) for i, s in enumerate(pseqs)]
    rep3 = PairTable(pd0 + pd1).integrity()
    sa3 = rep3.get("seq_advance", {})
    r.check(sa3.get("d0_park_entries") == 1,
            "a park is counted as ONE park entry, not one per parked frame",
            str(sa3))
    r.check(sa3.get("d0_held") == 3,
            "and the frames it sat at zero are counted as HELD",
            str(sa3))
    r.check(sa3.get("d0_skipped") == 0 and rep3.get("seq_advances_one_per_pair") is True,
            "a park is NOT reported as a skip (this is the fix)",
            str(sa3))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--vcd", default=DEFAULT_VCD)
    ap.add_argument("--sr", default=DEFAULT_SR)
    ap.add_argument("--skip-sr", action="store_true")
    args = ap.parse_args()

    r = Results()
    if shutil.which("sigrok-cli") is None:
        r.check(False, "sigrok-cli is installed",
                "install sigrok-cli to run the VCD and .sr checks")
        # The sequence-wrap checks use locally constructed frames and do not
        # need sigrok. Run them anyway so the failure is the only one.
        v1b_seq_wrap(r)
        return r.summary()

    v1_golden(args.vcd, r)
    v1b_seq_wrap(r)
    if not args.skip_sr:
        v2_sr_smoke(args.sr, r)
    v2b_absolute_path(r)
    v2c_decoder_flag(r)
    return r.summary()


if __name__ == "__main__":
    sys.exit(main())
