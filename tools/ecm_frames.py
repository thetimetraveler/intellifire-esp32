#!/usr/bin/env python3
"""Deterministic frame parser for the ECM <-> WFM serial link.

READ-ONLY ANALYSIS. Opens no hardware, defines no transmit path. Every frame it
emits is validated by MODBUS CRC-16, so a frame is either provably correct or
explicitly marked ambiguous/rejected — never guessed.

Frame format (confirmed against the 1 MHz capture — see docs/FRAME_FORMAT.md):

    4B | dev | seq | cmd | payload ... | CRC16-LE

    start   0x4B
    dev     endpoint; D0 carries 0x01/0x02, D1 carries 0x03/0x04 (= D0 + 2)
    seq     EXCHANGE counter, +1 per (D0,D1) pair. D0 and D1 share the value,
            which makes seq the join key between the two directions.
    cmd     operation byte
    CRC     MODBUS CRC-16, poly 0xA001 reflected, init 0xFFFF, computed over the
            frame from the start byte INCLUSIVE, stored little-endian.

Why this parser exists separately from tools/ecm_analyse.py
-----------------------------------------------------------
That script's scanner is entangled with print() calls, returns None, and caps
output at the first five matches. This module returns data.

It also differs in method. Rather than trying every length and taking the first
that validates, it:

  1. uses a length table seeded from the stable baseline, keyed by
     (channel, cmd) — NOT by cmd alone, because the reply direction uses
     different lengths for the same command byte:
         D0 BC ->  8 bytes    D1 BC -> 12 bytes
         D0 DC -> 31 bytes    D1 DC ->  8 bytes
         D0 6E ->  8 bytes    D1 6E -> 12 bytes
  2. falls back to a 6..80 sweep only for commands not in the table
  3. REQUIRES a structural boundary after the frame — either the next byte is
     another 0x4B, or there is a long idle gap before the next frame. This is
     what kills false positives, and it is the check ecm_analyse.py lacks.

Usage as a library:
    from ecm_frames import parse_bytes, decode_channel, PairTable
    frames = parse_bytes(byte_list, channel="D0")   # needs inter-byte gaps
    tab    = PairTable(frames); tab.integrity()     # seq / dev cross-checks

Usage as a CLI:
    ./ecm_frames.py logic_analyzer_data/dec3_D0.txt --channel D0
    ./ecm_frames.py logic_analyzer_data/dump3.vcd --channel D0   # decodes via sigrok
    ./ecm_frames.py logic_analyzer_data/dump3.vcd               # both channels
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, asdict, field

# --- reuse the verified CRC rather than reimplementing it ---------------------
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ecm_analyse import crc16_modbus  # noqa: E402

START = 0x4B
BAUD = 9600

# Frame length by (channel, cmd), seeded from the 1 MHz capture. The reply
# direction is shorter for DC (an ack) and longer for BC/6E (padding).
# Every entry here is confirmed against live captures.
KNOWN_LEN: dict[tuple[str, int], int] = {
    ("D0", 0xBC): 8,
    ("D0", 0xDC): 31,
    ("D0", 0x64): 9,
    ("D0", 0x6E): 8,
    ("D1", 0xBC): 12,
    ("D1", 0xDC): 8,
    ("D1", 0x64): 8,
    ("D1", 0x6E): 9,
}

MIN_FRAME_LEN = 6
MAX_FRAME_LEN = 80

# A UART character is 10 bit-periods. At 9600 baud that is ~1.042 ms, so a
# gap beyond ~5 characters means the bus went idle rather than the frame
# simply continuing.
SEC_PER_BYTE = 10.0 / BAUD
IDLE_GAP_S = 5 * SEC_PER_BYTE
DEFAULT_GAP_S = 2 * SEC_PER_BYTE

# sigrok-cli prints one of these per decoded byte:
#     0.019821 UART: RX data: 4B
#     uart-1: 4B
#     '19925-20759 uart-1: 4B'   (with --protocol-decoder-samplenum)
RE_TIMED = re.compile(r"^\s*([0-9]*\.?[0-9]+)\s+.*?:\s*([0-9A-Fa-f]{2})\s*$")
RE_BARE = re.compile(r"^\s*(?:.*?:\s*)?([0-9A-Fa-f]{2})\s*$")
RE_SAMPLENUM = re.compile(r"^\s*(\d+)\s*-\s*(\d+)\s+.*?:\s*([0-9A-Fa-f]{2})\s*$")


@dataclass
class Frame:
    """One CRC-validated frame."""

    channel: str
    t: float | None = None          # seconds, None if the source had no timing
    dev: int = 0
    seq: int = 0
    cmd: int = 0
    payload: list[int] = field(default_factory=list)
    raw: list[int] = field(default_factory=list)
    crc: int = 0
    index: int = 0                  # position in the channel's frame sequence
    gap_before: float | None = None # seconds since the previous frame's start
    ambiguous: bool = False
    #: other lengths that also validated for this start offset
    alt_lengths: list[int] = field(default_factory=list)

    @property
    def key(self) -> tuple[str, int, int]:
        return (self.channel, self.dev, self.cmd)

    @property
    def key_s(self) -> str:
        return f"{self.channel}/0x{self.dev:02X}/0x{self.cmd:02X}"

    @property
    def hex(self) -> str:
        return " ".join(f"{b:02X}" for b in self.raw)

    def to_json(self) -> dict:
        d = asdict(self)
        d["key"] = self.key_s
        d["hex"] = self.hex
        d["dev"] = f"0x{self.dev:02X}"
        d["cmd"] = f"0x{self.cmd:02X}"
        return d


# --------------------------------------------------------------------------
# byte extraction
# --------------------------------------------------------------------------

def extract_bytes_with_times(text: str, samplerate: float | None = None
                             ) -> tuple[list[int], list[float | None]]:
    """Pull decoded bytes, and their timestamps where the source provides them.

    Handles the three shapes sigrok-cli emits:

        0.019821 UART: RX data: 4B        (timed — a PulseView/CSV export)
        19925-20759 uart-1: 4B            (--protocol-decoder-samplenum)
        uart-1: 4B                        (bare)

    Timestamps matter because the boundary check in find_frames() uses
    inter-frame idle gaps. Samplenum output is converted using `samplerate`
    (samples are taken at the capture rate); pass it, or those bytes get
    timestamps of None and the boundary check degrades to "accept".
    """
    vals: list[int] = []
    times: list[float | None] = []
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        m = RE_SAMPLENUM.match(line)
        if m:
            vals.append(int(m.group(3), 16))
            times.append(int(m.group(1)) / samplerate if samplerate else None)
            continue
        m = RE_TIMED.match(line)
        if m:
            vals.append(int(m.group(2), 16))
            times.append(float(m.group(1)))
            continue
        m = RE_BARE.match(line)
        if m:
            vals.append(int(m.group(1), 16))
            times.append(None)
    return vals, times


# --------------------------------------------------------------------------
# framing
# --------------------------------------------------------------------------

def _crc_ok(raw: list[int], length: int) -> bool:
    if length < MIN_FRAME_LEN or len(raw) < length:
        return False
    body = raw[: length - 2]
    want = raw[length - 2] | (raw[length - 1] << 8)  # little-endian
    return crc16_modbus(bytes(body)) == want


def _boundary_ok(idx: int, start: int, length: int, raw: list[int],
                 times: list[float | None], idle_gap_s: float) -> bool:
    """Is a frame of `length` at `start` followed by a plausible boundary?

    A frame must end at a byte boundary, so the next byte is either another
    start byte, or arrives after the bus has gone idle. Without this check a
    payload byte of 0x4B can start a spurious short match.
    """
    nxt = start + length
    if nxt >= len(raw):
        return True                     # end of capture — accept
    if raw[nxt] == START:
        return True                     # immediately followed by a new frame
    t_end = times[nxt - 1] if nxt - 1 < len(times) else None
    t_next = times[nxt]
    if t_end is not None and t_next is not None:
        return (t_next - t_end) >= idle_gap_s
    return True                         # no timing — cannot refute, accept


def find_frames(raw: list[int], times: list[float | None] | None = None,
                channel: str = "D0", idle_gap_s: float = IDLE_GAP_S,
                strict: bool = False) -> list[Frame]:
    """Find all CRC-validated frames in a flat byte stream.

    `raw` must be the concatenation of every decoded byte on one channel, in
    order. `times` (same length) enables the idle-gap boundary check.
    """
    if times is None:
        times = [None] * len(raw)
    frames: list[Frame] = []
    start = 0
    n = len(raw)
    while start < n - MIN_FRAME_LEN:
        if raw[start] != START:
            start += 1
            continue

        cands: list[int] = []
        # 1. the table entry for this (channel, cmd), if we have one
        if start + 4 <= n:
            cmd = raw[start + 3]
            known = KNOWN_LEN.get((channel, cmd))
            if known is not None and _crc_ok(raw[start:], known):
                cands.append(known)
        # 2. a brute-force sweep for anything else
        if not cands:
            for length in range(MIN_FRAME_LEN, MAX_FRAME_LEN + 1):
                if start + length > n:
                    break
                if _crc_ok(raw[start:], length):
                    cands.append(length)

        # 3. structural boundary check, which is what removes false positives
        good = [L for L in cands if _boundary_ok(start, start, L, raw, times, idle_gap_s)]
        if not good and cands and not strict:
            good = cands

        if not good:
            start += 1
            continue

        length = good[0]
        raw_f = raw[start:start + length]
        t = times[start]
        gap = None
        if t is not None and frames and frames[-1].t is not None:
            gap = t - frames[-1].t
        frames.append(Frame(
            channel=channel,
            t=t,
            dev=raw_f[1],
            seq=raw_f[2],
            cmd=raw_f[3],
            payload=raw_f[4:length - 2],
            raw=raw_f,
            crc=raw_f[-2] | (raw_f[-1] << 8),
            index=len(frames),
            gap_before=gap,
            ambiguous=len(good) > 1,
            alt_lengths=good[1:],
        ))
        start += length
    return frames


def parse_bytes(raw: list[int], times: list[float | None] | None = None,
                channel: str = "D0", **kw) -> list[Frame]:
    return find_frames(raw, times, channel, **kw)


# --------------------------------------------------------------------------
# sigrok decode + file loading
# --------------------------------------------------------------------------

def sigrok_decode(path: str, channel: str, baud: int = BAUD,
                  show_time: bool = True) -> str:
    """Decode one channel of a .sr/.vcd with sigrok-cli.

    `path` MUST be absolute: the sigrok-cli wrapper cd's into its app bundle,
    so a relative path fails with "Not a regular file". The flag is
    -P/--protocol-decoders (plural) — the singular form is not valid.
    """
    if not os.path.isabs(path):
        raise ValueError(f"path must be absolute (sigrok-cli cd's into its bundle): {path}")
    cmd = ["sigrok-cli", "-i", path,
           "-P", f"uart:baudrate={baud}:rx={channel}",
           "-A", "uart=rx-data"]
    if show_time:
        cmd.append("--protocol-decoder-samplenum")
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    err = (r.stderr or "").strip().splitlines()
    err = [e for e in err if "Suspiciously low overall change rate" not in e
           and "Suggest to downsample" not in e]
    if err and not r.stdout.strip():
        raise RuntimeError("sigrok-cli: " + " | ".join(err))
    return r.stdout


def load_channel(path: str, channel: str, baud: int = BAUD,
                 use_local_decoder: bool = False) -> tuple[list[int], list[float | None]]:
    """Load decoded bytes for a channel, with real timestamps where possible.

    sigrok-cli is the primary decoder — it reproduces the known frames exactly
    on dump3.vcd (55 bytes on D0 = 8+31+8+8). `tools/uart_from_csv.py` is an
    optional fallback (pass use_local_decoder=True), but it is a
    pure-Python approximation and was producing wrong bytes on this VCD.
    """
    if path.lower().endswith((".sr", ".vcd")):
        samplerate = None
        try:
            samplerate = _sr_samplerate(path)
        except Exception:
            pass
        text = sigrok_decode(path, channel, baud)
        if text.strip():
            return extract_bytes_with_times(text, samplerate)
        if not use_local_decoder:
            return [], []

    if path.lower().endswith(".vcd") and use_local_decoder:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "uart_from_csv", os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                          "uart_from_csv.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        samples = list(mod.samples_from_vcd(path, channel))
        if samples:
            decoded = mod.decode_uart(sorted(samples, key=lambda x: x[0]), baud)
            return [b for _, b in decoded], [t for t, _ in decoded]

    with open(path, encoding="utf-8", errors="replace") as fh:
        return extract_bytes_with_times(fh.read())


def _sr_samplerate(path: str) -> float | None:
    """Read the capture sample rate, to convert samplenum output to seconds.

    sigrok-cli --show prints e.g. "Samplerate: 1000000" (no unit suffix) or
    "Samplerate: 1 MHz". Both shapes are handled.
    """
    r = subprocess.run(["sigrok-cli", "-i", os.path.abspath(path), "--show"],
                       capture_output=True, text=True, timeout=120)
    for line in r.stdout.splitlines():
        if "Samplerate" in line:
            m = re.search(r"([0-9.]+)\s*(k|m|g)?\s*hz\b", line, re.I)
            if m:
                mult = {"": 1, "k": 1e3, "m": 1e6, "g": 1e9}[(m.group(2) or "").lower()]
                return float(m.group(1)) * mult
            m = re.search(r":\s*([0-9.]+)\s*$", line)
            if m:
                return float(m.group(1))
    return None


def decode_channel(path: str, channel: str, **kw) -> list[Frame]:
    raw, times = load_channel(path, channel)
    return find_frames(raw, times, channel, **kw)


# --------------------------------------------------------------------------
# integrity: pairing D0 and D1
# --------------------------------------------------------------------------

@dataclass
class PairTable:
    """Cross-checks between the two directions of the link.

    Establishes, from the data rather than from assumption:
      * seq is an EXCHANGE counter — each value appears once on D0 and once on D1
      * D1 replies to D0, one frame duration later
      * D1's device ID is D0's + 2
    """

    frames: list[Frame]

    def by_channel(self) -> dict[str, list[Frame]]:
        out: dict[str, list[Frame]] = {}
        for f in self.frames:
            out.setdefault(f.channel, []).append(f)
        return out

    def integrity(self) -> dict:
        ch = self.by_channel()
        d0 = sorted(ch.get("D0", []), key=lambda f: (f.t is None, f.t))
        d1 = sorted(ch.get("D1", []), key=lambda f: (f.t is None, f.t))
        report: dict = {
            "frames": len(self.frames),
            "per_channel": {k: len(v) for k, v in ch.items()},
            "crc_valid_ratio": 1.0,   # every returned frame is CRC-valid by construction
        }

        # dev ranges
        report["dev_values"] = {
            k: sorted({f.dev for f in v}) for k, v in ch.items()
        }
        report["cmd_values"] = {
            k: sorted({f.cmd for f in v}) for k, v in ch.items()
        }

        # seq pairing
        s0 = [f.seq for f in d0]
        s1 = [f.seq for f in d1]
        set0, set1 = set(s0), set(s1)
        report["seq"] = {
            "d0": [f"0x{v:02X}" for v in s0],
            "d1": [f"0x{v:02X}" for v in s1],
            "d0_only": [f"0x{v:02X}" for v in sorted(set0 - set1)],
            "d1_only": [f"0x{v:02X}" for v in sorted(set1 - set0)],
            "shared": [f"0x{v:02X}" for v in sorted(set0 & set1)],
            "each_shared_once_per_direction": (
                all(s0.count(v) == 1 and s1.count(v) == 1 for v in (set0 & set1))
            ),
        }

        # dev offset D1 = D0 + 2, checked per shared seq
        offsets: dict[int, int] = {}
        for v in sorted(set0 & set1):
            f0 = next(f for f in d0 if f.seq == v)
            f1 = next(f for f in d1 if f.seq == v)
            offsets.setdefault(f1.dev - f0.dev, 0)
            offsets[f1.dev - f0.dev] += 1
        report["dev_offset_histogram"] = {str(k): n for k, n in sorted(offsets.items())}
        report["dev_offset_is_2"] = set(offsets) == {2} if offsets else None

        # Is seq monotonic +1 per exchange, in TIME ORDER?
        #
        # It MUST be measured on the time-ordered frame sequence, not on the
        # sorted set of distinct values. The counter is one byte and therefore
        # WRAPS at 256 (0xFF -> 0x00); over a capture long enough to cross the
        # wrap, `sorted(set(...))` puts the low values first and manufactures a
        # single huge bogus delta at the wrap point. That produced a false
        # SEQ_BROKEN verdict on the first live 60 s capture, where the real
        # sequence was a perfect 160/160 of +1.
        def _steps(seqs: list[int]) -> list[int]:
            return [(seqs[i + 1] - seqs[i]) & 0xFF for i in range(len(seqs) - 1)]

        def _classify(seqs: list[int]) -> tuple[int, int, int]:
            """Split deviations into HELD, RESET-TO-ZERO, and SKIPPED.

            Conflating these inflated the reported anomaly count by more than an
            order of magnitude, and then a second time. Three distinct things:

              HELD    (delta 0) — the counter did not advance. The ECM does this
                                    deliberately: on a reply timeout it PARKS the
                                    counter at 0 and holds it while it keeps
                                    polling. ONE stall produces a RUN of held
                                    frames. Counting each pair inside the run
                                    reported a single stall as 27 'off-by's.
              RESET   (delta != 1, lands on 0) — the entry into a park. Expected
                                    after a reply timeout, and indistinguishable
                                    from a deliberate counter reset, because it
                                    IS one.
              SKIPPED (anything else) — a genuine discontinuity: frames lost, or
                                    a counter that jumped somewhere other than to
                                    zero. THIS is the only one that is anomalous.

            Observed across four captures: a handful of RESETs (one per stall),
            tens of HELDs (frames spent parked), and SKIPPED equal to the RESET
            count — i.e. every apparent 'skip' was a park entry, and there were
            no unexplained losses at all.
            """
            held = sum(1 for i in range(len(seqs) - 1)
                       if (seqs[i + 1] - seqs[i]) & 0xFF == 0)
            reset = sum(1 for i in range(len(seqs) - 1)
                        if (d := (seqs[i + 1] - seqs[i]) & 0xFF) != 1
                        and d != 0 and seqs[i + 1] == 0)
            skipped = sum(1 for i in range(len(seqs) - 1)
                          if (d := (seqs[i + 1] - seqs[i]) & 0xFF) not in (0, 1)
                          and seqs[i + 1] != 0)
            return held, reset, skipped

        steps0, steps1 = _steps(s0), _steps(s1)
        held0, reset0, skip0 = _classify(s0)
        held1, reset1, skip1 = _classify(s1)
        report["seq_advance"] = {
            "d0_steps": len(steps0), "d0_held": held0,
            "d0_park_entries": reset0, "d0_skipped": skip0,
            "d1_steps": len(steps1), "d1_held": held1,
            "d1_park_entries": reset1, "d1_skipped": skip1,
            "wrap_observed": (max(s0 + s1) == 0xFF and min(s0 + s1) == 0x00)
                             if (s0 or s1) else False,
        }
        # The counter genuinely fails to advance by one only when it SKIPS.
        # Holding is the ECM parked after a reply timeout — observed and benign;
        # a park entry is that same event's first frame.
        report["seq_advances_one_per_pair"] = (
            (skip0 == 0 and skip1 == 0) if (steps0 or steps1) else None
        )

        # Distinct-value view, kept for readability (NOT used for the verdict).
        shared = sorted(set0 & set1)
        report["seq_pair_deltas"] = _steps(shared) if len(shared) > 1 else []

        # reply latency: D1 start minus D0 start, versus D0 frame duration
        lat = []
        for v in shared:
            f0 = next(f for f in d0 if f.seq == v)
            f1 = next(f for f in d1 if f.seq == v)
            if f0.t is not None and f1.t is not None:
                dur0 = len(f0.raw) * SEC_PER_BYTE
                lat.append({
                    "seq": f"0x{v:02X}",
                    "delta_ms": round((f1.t - f0.t) * 1000, 3),
                    "d0_duration_ms": round(dur0 * 1000, 3),
                    "ratio": round((f1.t - f0.t) / dur0, 3) if dur0 else None,
                })
        report["reply_latency"] = lat
        return report


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help=".sr / .vcd capture, or a decoded text dump")
    ap.add_argument("--channel", default=None,
                    help="D0 or D1 (default: both, for .sr/.vcd)")
    ap.add_argument("--baud", type=float, default=BAUD)
    ap.add_argument("--json", action="store_true", help="emit frames as JSON lines")
    ap.add_argument("--no-strict", action="store_true",
                    help="accept a frame even if no structural boundary confirms it")
    args = ap.parse_args()

    path = os.path.abspath(args.capture)
    channels = [args.channel] if args.channel else (
        ["D0", "D1"] if path.lower().endswith((".sr", ".vcd")) else ["D0"])

    all_frames: list[Frame] = []
    for ch in channels:
        try:
            raw, times = load_channel(path, ch, int(args.baud))
        except Exception as exc:
            print(f"# {ch}: {exc}", file=sys.stderr)
            continue
        frames = find_frames(raw, times, ch, strict=not args.no_strict)
        all_frames.extend(frames)
        print(f"# {ch}: {len(raw)} bytes -> {len(frames)} CRC-valid frames",
              file=sys.stderr)
        if args.json:
            for f in frames:
                print(json.dumps(f.to_json()))
        else:
            for f in frames:
                t = f"{f.t:.6f} " if f.t is not None else ""
                amb = "  !AMBIGUOUS" if f.ambiguous else ""
                print(f"{t}{f.key_s} seq=0x{f.seq:02X} "
                      f"pay={len(f.payload):2d}  {f.hex}{amb}")

    if not args.json and len(channels) > 1 and all_frames:
        tab = PairTable(all_frames)
        print("\n=== INTEGRITY ===")
        print(json.dumps(tab.integrity(), indent=2))


if __name__ == "__main__":
    main()
