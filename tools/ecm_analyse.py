#!/usr/bin/env python3
"""Analyse a captured ECM <-> WFM serial link.

Takes either:
  - a sigrok .sr file (decodes UART @9600 with sigrok-cli, then analyses), or
  - a text file / stdin of already-decoded UART output

It then hunts for the frame structure hypothesised in HA thread post 296:
  start byte 0x4B, device, sequence, command, payload, MODBUS CRC-16 little-endian

READ-ONLY ANALYSIS. This script never transmits and never touches the hardware.

Usage:
    ./ecm_analyse.py capture.sr
    ./ecm_analyse.py decoded.txt
    sigrok-cli -i capture.sr -P uart:baudrate=9600:rx=D0 -A uart=rx-data | ./ecm_analyse.py -
"""
import collections
import os
import re
import shutil
import subprocess
import sys


def crc16_modbus(data: bytes) -> int:
    """MODBUS CRC-16: poly 0xA001 (reflected), init 0xFFFF."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def load_from_sr(path: str) -> str:
    """Run sigrok-cli to decode UART on each available channel.

    The flag is -P / --protocol-decoders (plural); the singular
    --protocol-decoder is not a valid option and fails with "Unknown option".
    `path` MUST be absolute — the sigrok-cli wrapper cd's into its app bundle,
    so a relative path fails with "Not a regular file".
    """
    if not shutil.which("sigrok-cli"):
        sys.exit("sigrok-cli not found — install with: brew install sigrok-cli")
    if not os.path.isabs(path):
        sys.exit(f"path must be absolute (sigrok-cli cd's into its bundle): {path}")
    out = []
    for ch in ("D0", "D1", "D2", "D3"):
        try:
            r = subprocess.run(
                ["sigrok-cli", "-i", path,
                 "-P", f"uart:baudrate=9600:rx={ch}",
                 "-A", "uart=rx-data"],
                capture_output=True, text=True, timeout=300,
            )
            if r.stdout.strip():
                out.append(f"### channel {ch}\n{r.stdout}")
        except subprocess.TimeoutExpired:
            out.append(f"### channel {ch}: decode timed out")
    return "\n".join(out) if out else ""


HEXBYTE = re.compile(r"\b([0-9A-Fa-f]{2})\b")

# Longest plausible frame: the DC status frame observed on this bus is 31 bytes.
# An earlier limit of 24 silently skipped it.
MAX_FRAME_LEN = 80


def extract_bytes(text: str) -> list[int]:
    """Pull hex byte values out of decoded UART text."""
    vals = []
    for m in HEXBYTE.finditer(text):
        vals.append(int(m.group(1), 16))
    return vals


def analyse(text: str) -> None:
    if not text.strip():
        print("No decoded data to analyse.")
        return

    # Show the raw decoded text so a human can sanity-check it
    print("=" * 60)
    print("DECODED OUTPUT (first 3 kB)")
    print("=" * 60)
    print(text[:3000])
    print()

    bytes_ = extract_bytes(text)
    if not bytes_:
        print("No hex byte values found. The channel may be idle, or the baud rate")
        print("may be wrong. Check that the channel is actually toggling.")
        return

    print("=" * 60)
    print(f"FRAME ANALYSIS  ({len(bytes_)} byte tokens seen)")
    print("=" * 60)

    # 1. Start-byte hypothesis
    starts = [b for b in bytes_ if b == 0x4B]
    print(f"\n0x4B occurrences: {len(starts)}  "
          f"({100*len(starts)/len(bytes_):.1f}% of tokens)")
    print("  -> post 296 predicts 0x4B as a frame start.")

    # 2. Most common byte values
    common = collections.Counter(bytes_).most_common(10)
    print("\nMost common byte values:")
    for val, n in common:
        tag = "   <-- 0x4B start byte hypothesis" if val == 0x4B else ""
        print(f"  0x{val:02X}  x{n}{tag}")

    # 3. Try CRC-16 over plausible frame lengths
    print("\nMODBUS CRC-16 check (poly 0xA001, little-endian trailing CRC):")
    found = 0
    for start in range(len(bytes_) - 6):
        if bytes_[start] != 0x4B:
            continue
        for flen in range(6, MAX_FRAME_LEN):
            end = start + flen
            if end >= len(bytes_):
                break
            body, crc_bytes = bytes_[start:end - 2], bytes_[end - 2:end]
            crc_calc = crc16_modbus(bytes(body))
            crc_frame = crc_bytes[0] | (crc_bytes[1] << 8)  # little-endian
            if crc_calc == crc_frame:
                found += 1
                if found <= 5:
                    print(f"  ✅ MATCH len={flen}: "
                          f"{' '.join(f'{b:02X}' for b in bytes_[start:end])}")
                break
    if found:
        print(f"\n  {found} frame(s) validated by MODBUS CRC-16.")
        print("  🎯 This CONFIRMS the post-296 frame hypothesis.")
    else:
        print("  No CRC-validated frames found.")
        print("  Possibilities: (a) not the right channel, (b) baud mismatch,")
        print("  (c) different frame structure, (d) CRC is not MODBUS.")

    # 4. Sequence-number check: look for a steadily incrementing byte
    print("\nLooking for an incrementing sequence counter...")
    deltas = collections.Counter()
    for i in range(len(bytes_) - 1):
        d = (bytes_[i + 1] - bytes_[i]) & 0xFF
        if d in (1, 2):
            deltas[d] += 1
    if deltas:
        print(f"  byte-to-byte increments of +1/+2: {sum(deltas.values())} occurrences")
    else:
        print("  none obvious")

    print("\n" + "=" * 60)
    print("NEXT STEPS IF NOTHING DECODED")
    print("=" * 60)
    print("  1. Confirm the channel toggles (raw edge check in ecm_capture.sh)")
    print("  2. Try other baud rates: 19200, 38400, 57600, 115200")
    print("  3. Try the other channel — you may have the ECM's RX, not TX")
    print("  4. Invert: some links idle LOW (add ':invert=1' to the decoder)")


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        sys.exit(__doc__)
    path = sys.argv[1]
    if path == "-":
        analyse(sys.stdin.read())
    elif path.endswith(".sr"):
        print(f"Decoding {path} with sigrok-cli...\n")
        analyse(load_from_sr(path))
    else:
        analyse(open(path, encoding="utf-8", errors="replace").read())


if __name__ == "__main__":
    main()
