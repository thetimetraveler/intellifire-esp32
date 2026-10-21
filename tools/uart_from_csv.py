#!/usr/bin/env python3
"""Decode UART from a raw logic capture (VCD or CSV), with no sigrok dependency.

Why this exists: this is the pure-Python fallback decoder. `tools/ecm_frames.py`
prefers sigrok-cli as the primary decoder -- sigrok-cli IS installed on this
machine (~/.local/bin/sigrok-cli, 0.8.0-git via Homebrew) and reproduces the
known dump3.vcd frames exactly -- while this module stays as the
dependency-free offline path for captures and machines without sigrok.

The canonical sigrok invocation uses the PLURAL -P/--protocol-decoders flag
(the singular --protocol-decoder fails with "Unknown option"):

    sigrok-cli -i /abs/path/dump.vcd \\
        -P uart:baudrate=9600:rx=D0 -A uart=rx-data --protocol-decoder-samplenum

The input path must be ABSOLUTE: the sigrok-cli wrapper cd's into its app
bundle, so a relative path fails with "Not a regular file".

One timing note drives the decoder design below. Nominal 9600 baud implies a
104.1667 us bit period, but this link actually runs at 104 us per bit
(~9615 baud). UART resynchronises on every start bit, so that -0.16% error
never corrupts a byte; accumulated across a whole frame it still drifts enough
to matter at frame *boundaries* -- see decode_uart.

LISTEN-ONLY: this is pure offline analysis of already-captured data. It opens no
hardware and defines no transmit path.

Usage:
    ./uart_from_csv.py capture.csv --baud 9600 --channel D0
    ./uart_from_csv.py capture.vcd --baud 9600 --channel D0
    ./uart_from_csv.py capture.vcd --baud 9600 --channel D0 --invert
    ./uart_from_csv.py capture.vcd --baud 9600 --channel D0 --estimate-baud

Emits decoded bytes in the same style as sigrok-cli, so its output can be piped
into ecm_analyse.py:
    ./uart_from_csv.py capture.csv --baud 9600 --channel D0 | ./ecm_analyse.py -
"""
import argparse
import csv
import re
import sys


def samples_from_csv(path, channel, limit=None):
    """Yield (time_seconds, bool) from a PulseView CSV export.

    Handles the common PulseView shapes:
      - a header row containing channel names, then rows of 0/1
      - rows of "<time>, <sample>, <sample>..."
    """
    with open(path, newline="", encoding="utf-8", errors="replace") as fh:
        rows = list(csv.reader(fh))
    if not rows:
        return

    header = [c.strip() for c in rows[0]]
    # Does the header name channels?
    idx = None
    for i, h in enumerate(header):
        if h.lower() == channel.lower().strip():
            idx = i
            break
    start = 0
    if idx is None:
        # No usable header: assume col 0 = time, col 1 = first channel
        print(f"# note: channel '{channel}' not in header {header}; using column 1",
              file=sys.stderr)
        idx = 1
    else:
        start = 1

    for r in rows[start:]:
        if len(r) <= idx:
            continue
        v = r[idx].strip()
        if v in ("", "-"):
            continue
        try:
            t = float(r[0]) if r[0].strip() else 0.0
        except ValueError:
            t = 0.0
        yield t, (v not in ("0", "0.0", "false", "False", "LOW", "low"))
        if limit and t > limit:
            return


def samples_from_vcd(path, channel, limit=None):
    """Yield (time_seconds, bool) from a VCD file.

    Handles the libsigrok/PulseView shape, where a timestamp line may also carry
    value changes on the SAME line, e.g.:

        #0 1! 1"
        #19821 0!

    Each token after an optional `#<time>` is either `0<id>` or `1<id>` for a
    1-bit wire, or `b<bits> <id>` for a vector.
    """
    scale = {"s": 1.0, "ms": 1e-3, "us": 1e-6, "ns": 1e-9, "ps": 1e-12}
    t_scale = 1e-6
    ident = None       # VCD id for the channel of interest
    t = 0.0
    with open(path, encoding="utf-8", errors="replace") as fh:
        for raw in fh:
            line = raw.strip()
            if not line:
                continue
            if line.startswith("$timescale"):
                m = re.search(r"(\d+)\s*([a-z]+)", line)
                if m:
                    t_scale = int(m.group(1)) * scale.get(m.group(2), 1e-6)
                continue
            if line.startswith("$var"):
                # $var wire 1 ! D0 $end   ->  ident='!', name='D0'
                parts = line.split()
                if len(parts) >= 5 and parts[4].lower() == channel.lower():
                    ident = parts[3]
                continue
            if line.startswith("$"):
                continue

            # A line may be:  "#<time> <change> <change> ..."  or just changes.
            tokens = line.split()
            if tokens and tokens[0].startswith("#"):
                try:
                    t = int(tokens[0][1:]) * t_scale
                except ValueError:
                    pass
                tokens = tokens[1:]
                if limit and t > limit:
                    return

            for tok in tokens:
                if len(tok) >= 2 and tok[0] in "01":
                    if tok[1:] == ident:
                        yield t, (tok[0] == "1")
                elif tok.startswith("b") and ident is not None and tok.endswith(ident):
                    bits = tok[1:-len(ident)] if ident else tok[1:]
                    yield t, bits.strip().endswith("1")


def estimate_baud(samples, min_interval=20e-6, max_interval=200e-6):
    """Estimate the real baud rate from the shortest edge interval, or None.

    UART data bit periods show up as the shortest run of edge-to-edge intervals
    on the line, so the *median* of the intervals near the minimum is a robust
    estimator: a single glitch can't drag it, and long idle gaps sit far above
    the noise floor. The window is deliberately narrow so long intervals
    (idle gaps, whole frames) can never win.

    Returns None when the capture has too few edges to judge, in which case the
    caller should keep the nominal baud.
    """
    ts = [t for t, _ in samples]
    intervals = [b - a for a, b in zip(ts, ts[1:])
                 if min_interval <= b - a <= max_interval]
    if len(intervals) < 8:
        return None
    intervals.sort()
    # Median of the fastest decile: below this the estimate is too noisy,
    # above it whole frames start creeping in.
    k = max(1, len(intervals) // 10)
    return 1.0 / intervals[k // 2]


def decode_uart(samples, baud, invert=False, max_bytes=5000, estimate=False):
    """Decode 8-N-1 UART from a sample stream. Returns list of (t, byte).

    `samples` must be sorted by time. Assumes the line idles at `idle` (high unless
    inverted). A frame is 1 start bit + 8 data bits (LSB first) + 1 stop bit.

    Decoding is done by jumping an iterator forward past each frame, so edges that
    occur *within* a frame are never mistaken for new start bits.

    Frame-boundary handling is the subtle part. A nominal-baud frame end
    (start_t + 10 * bit_ns) is only correct if the link really runs at `baud`.
    When it runs slightly fast -- as this link does, 104 us bits against a
    nominal 104.1667 us -- the nominal frame end lands *after* the next frame's
    real start bit, so a naive "skip to frame_end" step eats that start edge and
    every following byte loses its start bit (dump3.vcd collapsed from 55 bytes
    to 33 this way). The remedy is to skip only as far as the next frame's start
    bit could possibly begin -- the stop bit ends at 9.5 bit periods, and a
    receiver tolerates roughly half a bit of early start, so stop half a bit
    short of the nominal frame end -- and then let the ordinary start-edge scan
    take over. Intra-frame edges still can't be mistaken for start bits, because
    the scan resumes after the stop bit.

    `estimate=True` derives the bit period from the data (see estimate_baud)
    instead of trusting the nominal baud. It is OFF by default so existing
    behaviour stays predictable; the skip tolerance is already proportional to
    bit_ns, so the nominal path degrades gracefully without it.
    """
    out = []
    dense = samples if isinstance(samples, list) else list(samples)
    n = len(dense)
    if n < 2:
        return out

    bit_ns = 1.0 / baud
    if estimate:
        real = estimate_baud(dense)
        if real:
            bit_ns = 1.0 / real
    # The line idles HIGH. With `invert`, the physical line idles LOW instead
    # (an inverting transceiver or an idle-low bus), so idle_level tracks the
    # *raw sample* value we expect to see while idle. All comparisons below are
    # against raw sample values, so no separate flipping step is needed.
    idle_level = not invert

    def level_at(target_t):
        """Raw level at time target_t, or None if outside the capture."""
        if target_t < dense[0][0] or target_t > dense[-1][0]:
            return None
        lo, hi = 0, n - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if dense[mid][0] <= target_t:
                lo = mid
            else:
                hi = mid - 1
        return dense[lo][1]

    # Walk the sample list looking for start bits.
    i = 1
    while i < n and len(out) < max_bytes:
        prev_l = dense[i - 1][1]
        cur_t, cur_l = dense[i]

        # A start bit is an edge away from idle.
        if prev_l == idle_level and cur_l != idle_level:
            start_t = cur_t
            val = 0
            ok = True
            for b in range(8):
                centre = start_t + (1.5 + b) * bit_ns
                v = level_at(centre)
                if v is None:
                    ok = False
                    break
                # UART: a HIGH line is a 1, LOW is a 0 (relative to idle).
                if v == idle_level:
                    val |= (1 << b)          # LSB first
            # Validate the stop bit
            stop_c = start_t + 9.5 * bit_ns
            sv = level_at(stop_c)

            # A capture can end mid-frame with the stop bit cut off. All 8 data
            # bits are still present and were already read above, so such a
            # truncated final frame is reported rather than dropped -- sigrok
            # decodes it too. This can only ever add the last byte of a
            # capture: truncated_end requires the stop-bit centre to fall past
            # the final sample, so no byte in mid-capture qualifies.
            truncated_end = ok and stop_c > dense[-1][0]

            if (ok and sv is not None and sv == idle_level) or truncated_end:
                # Skip the iterator past this frame so intra-frame edges are not
                # re-detected as start bits -- but stop HALF A BIT SHORT of the
                # nominal frame end. The stop bit spans 9.5..10 bit periods, so
                # 9.5 is the earliest the next start bit can begin; skipping a
                # full 10*bit_ns overshoots on a link running slightly fast and
                # eats the next byte's start edge (see docstring).
                resume_t = start_t + 9.5 * bit_ns
                j = i
                while j < n and dense[j][0] < resume_t:
                    j += 1
                if j < n and dense[j][1] != idle_level:
                    # Already low at the stop-bit centre: the next start bit may
                    # be in progress. Step back to the edge that produced that
                    # low level so the outer scan sees it as a start bit instead
                    # of skipping over it. A false start here is harmless -- it
                    # fails the stop-bit check on the next iteration and the
                    # scan simply continues.
                    while j > 0 and dense[j - 1][1] != idle_level:
                        j -= 1
                i = j
                out.append((start_t, val))
                continue

        i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture", help="CSV or VCD exported from PulseView")
    ap.add_argument("--baud", type=float, default=9600)
    ap.add_argument("--channel", default="D0")
    ap.add_argument("--invert", action="store_true",
                    help="use if the line idles LOW")
    ap.add_argument("--limit", type=float, default=None,
                    help="only decode the first N seconds")
    ap.add_argument("--estimate-baud", action="store_true",
                    help="derive the bit period from the data instead of "
                         "trusting --baud (off by default; see decode_uart)")
    args = ap.parse_args()

    loader = samples_from_vcd if args.capture.lower().endswith(".vcd") else samples_from_csv
    print(f"# loading {args.capture} channel={args.channel}", file=sys.stderr)
    samples = list(loader(args.capture, args.channel, args.limit))
    print(f"# {len(samples)} samples, "
          f"{samples[0][0]:.3f}s..{samples[-1][0]:.3f}s" if samples else "# no samples",
          file=sys.stderr)
    if not samples:
        sys.exit("no samples loaded — check the channel name and file format")

    # Normalise to a dense list for binary search
    dense = sorted(samples, key=lambda x: x[0])
    decoded = decode_uart(dense, args.baud, args.invert,
                          estimate=args.estimate_baud)

    note = ""
    if args.estimate_baud:
        real = estimate_baud(dense)
        if real:
            note = f" (bit period estimated: {real:g} baud)"
    print(f"# decoded {len(decoded)} bytes at {args.baud:g} 8-N-1{note}",
          file=sys.stderr)
    for t, b in decoded:
        print(f"{t:.6f} UART: RX data: {b:02X}")


if __name__ == "__main__":
    main()
