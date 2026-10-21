# Capture data

Seven paired recordings of the ECM ↔ Wi-Fi-module serial link, used by the
replay test:

| Run | Purpose |
|---|---|
| `preserved-run-manual-0319` | preserved run |
| `run-manual` | idle / manual run |
| `run-manual-20261016-103601` | manual run |
| `run-manual-20261016-112657` | manual run |
| `run-manual-20261016-152343` | manual run |
| `run-manual-20261016-153329` | manual run |
| `run-manual-20261016-154604` | manual run |

Each directory contains exactly two files:

- `frames_D0.jsonl` — the ECM's transmit line (ECM pin 6).
- `frames_D1.jsonl` — the module's transmit line (ECM pin 4).

`D0` and `D1` together are what makes direction and request/reply pairing
unambiguous. A single line is not enough.

## Record format

One JSON object per frame:

| Field | Meaning |
|---|---|
| `channel` | `D0` or `D1` |
| `t` | seconds from the start of the recording (not wall-clock time) |
| `dev` | device/register byte |
| `seq` | exchange counter |
| `cmd` | command byte |
| `payload` | parsed payload bytes |
| `raw` | the complete frame bytes, including start and CRC |
| `crc` | stored CRC-16 value |
| `index` | frame index on its channel |
| `gap_before` | idle gap before this frame, seconds; `null` for the first |
| `ambiguous` | frame boundary was not unique |
| `alt_lengths` | alternative valid lengths, if any |
| `key` | convenience key, e.g. `D0/0x01/0xDC` |
| `hex` | the frame as spaced hexadecimal |

## What is deliberately not included

The capture directories in the reference project also contained `polls.jsonl`,
`run.json`, `capture.log`, `manifest.jsonl`, `summary.json`, raw `.sr` files, and
sweep indexes. Those files can contain the module's serial number, LAN addresses,
absolute paths, and capture commands. They are excluded here. Only the frame
records are published; the frames themselves contain no network or personal
identifiers.

## Replay

```sh
make -C firmware test-replay CAPTURES=../captures
```

Expected result:

```
compared 6145 exchanges: 6136 byte-identical, 9 module command injections
(correctly NOT reproduced), 224 unpaired, 0 MISMATCH
```

The 9 "injections" are exchanges where the real module wrote a command into the
`0xBC` reply slot. The responder has no pending command in those exchanges, so
emitting the idle slot is the correct output; they are counted separately rather
than as mismatches.

## Provenance

The recordings were made on the ECM's low-voltage 8-pin Wi-Fi port at 9600-8-N-1
with a logic analyzer. `D0` is the ECM's transmit line; `D1` is the module's
transmit line. The frame format is documented in
[../docs/FRAME_FORMAT.md](../docs/FRAME_FORMAT.md).
