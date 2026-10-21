# ECM ↔ WFM serial frame format — VERIFIED

**Status: confirmed mathematically, 2026-10-16.** This upgrades HA-thread post 296 from "one person's
unverified observation" to a **verified frame structure**.

Source of the hypothesis: a community capture posted in the Home Assistant forum thread on
IntelliFire Wi-Fi modules (post 296), reported to have been taken on this same port with no module
fitted.

> ## ⚠️⚠️ READ FIRST — the frame below is ECM → MODULE, and the direction is now RESOLVED
>
> **The frame format documented here is the `D0` line — ECM pin 6 — and `D0` is the ECM→module
> direction. The ECM transmits here; the module transmits on `D1` (ECM pin 4).** Both are now
> **confirmed by content**, not inferred from timing.
>
> **Why this document carries the correction:** the frames in it come from a **community capture
> reported to have been taken on this port with the module physically removed**. Frames that exist with
> no module fitted can only come from the ECM — so this format is the ECM's transmit format, and the
> community sample frames are byte-identical in shape to the `D0` `BC` frames in this project's 1 MHz
> capture.
>
> ### ⭐ What that means for an implementer
>
> ⚠️ **A board replacing the module must not transmit this format as a command.** The module is a
> **polled slave**: it never initiates a frame, and its only write channel is a **4-byte data slot
> inside its reply to the `BC` request** on `D1`. A reader who takes this document's `BC` frame as "the
> command to send" would build the wrong thing. **The full request/reply and write-path picture is in
> `docs/PROTOCOL_DECODED.md` and `docs/COMMAND_MAP.md`.**
>
> ⚠️ **Post 296 is an inherited claim**, not this project's measurement — but its frame format
> **CRC-validates against the reference unit's captures** and matches the decode byte-for-byte, which
> is why it is trusted for the format (and now for the direction).

---

## The frame

```
 4B 02 4C BC 02 04 A1 B7
 │  │  │  │  └─┬──┘ └─┬─┘
 │  │  │  │    │      └── CRC-16, little-endian
 │  │  │  │    └───────── payload
 │  │  │  └────────────── command
 │  │  └───────────────── sequence number
 │  └──────────────────── device / register
 └─────────────────────── start byte (0x4B)
```

| Field | Bytes | Notes |
|---|---|---|
| Start | 1 | **`0x4B`** — fixed |
| Device / register | 1 | `0x02` in both samples; `0x01` in others post 296 saw |
| Sequence | 1 | increments; the community capture saw `4C`, `02`, `9B`, `A7` |
| Command | 1 | `0xBC` in both samples; also `0xDC`, `0x48` |
| Payload | variable | `0x02 0x04` in the short samples; longer status dumps exist |
| CRC | 2 | **MODBUS CRC-16, little-endian** |

**Serial parameters: 9600-8-N-1.**

**Direction: this is the `D0` / ECM-pin-6 line = ECM→module** (confirmed — see the block above).

> ### ⭐ The `02 04` payload is a request, and it comes back echoed
>
> The `02 04` payload of this `BC` frame is the **request**. The module's reply on `D1` opens with the
> **same two bytes** — `4B 04 <seq> BC 02 04 …` — because **every reply echoes the first two payload
> bytes of its request**. Verified on **642/642 exchange pairs**.
>
> ⚠️ **So `02 04` is not a header or an opcode.** It is the request's first two payload bytes repeated
> back, and the module's *actual* reply data begins after it. (Likely explanation — a **hypothesis**, not
> a finding: the second byte is a byte-count, "area `02`, up to `04` bytes", which fits the fixed 4-byte
> data slot exactly.)

---

## ✅ Verification

Both of the community capture's published sample frames were run through the MODBUS CRC-16 algorithm
(polynomial `0xA001`, reflected, init `0xFFFF`) with the CRC computed over the frame **from `0x4B`
inclusive through the last payload byte**, then compared against the trailing two bytes read
**little-endian**:

| Frame | Computed CRC | Frame bytes (LE) | Result |
|---|---|---|---|
| `4B 02 4C BC 02 04` + `A1 B7` | `0xB7A1` | `0xB7A1` | ✅ **MATCH** |
| `4B 02 02 BC 02 04` + `B6 9F` | `0x9FB6` | `0x9FB6` | ✅ **MATCH** |

Big-endian interpretation fails both. **Two independent frames validating under one consistent
algorithm is not coincidence.**

### What this establishes

1. **`0x4B` is the start byte** — confirmed.
2. **CRC-16 with MODBUS polynomial `0xA001`** — confirmed.
3. **The CRC covers the whole frame from the start byte**, not payload-only — confirmed.
4. **CRC is stored little-endian** — confirmed.
5. **The community capture is credible**, which also strengthens the mirror pin mapping
   (`pin 3 = +5 V`, `pin 6 = TX`) derived from the same post in `ECM_PORT_PINOUT.md`.
6. ⭐ **That pin 6 is the ECM's serial TX** — **confirmed, not just plausible.** This project's capture
   of the same line decodes every frame CRC-clean at 9600-8-N-1, and the same port produces these
   frames with the module physically absent according to the community report.
   `ECM_PORT_PINOUT.md` may still label pin 6 a "candidate"; **this document settles it.**

### What it does NOT establish

> **Updated.** Three items below used to read "not yet tested" / "still unknown" / "single-source". They
> have since been resolved by this project's captures; recorded here so the older text is not read as
> current.

- ~~**Whether this project's capture matches.** Not yet tested — that's the next step.~~ **Resolved:**
  it matches. `dump3.vcd` (1 MHz) decodes cleanly with every frame CRC-validating, and the community
  frame `… 9B DC 00 17 00 00 00 00 00 04 …` appears in the reference capture as
  `… 2A DC 00 17 00 00 00 00 00 04 …` — the status payload is byte-for-byte identical.
- ~~**The command/payload semantics.** Still unknown.~~ **Partly resolved:** the `DC` **payload** is
  mapped (a state report — pilot, height, hot, power), and `0xE4`/`0xE1` are decoded as **ECM state
  announcements**. ⚠️ The **purpose of the four routine command bytes** (`BC`, `DC`, `64`, `6E`) is
  still unknown — **do not guess.**
- ~~**Baud rate independently.** `9600-8-N-1` is still single-source (post 296) …~~ **Now independently
  corroborated:** this project's captures at 1 MHz decode CRC-clean at **9600-8-N-1, lsb-first**, with
  **zero** valid `0x4B` frames under msb-first.

---

## Tooling

`tools/ecm_analyse.py` implements this check. It:

- decodes a sigrok `.sr` capture with `sigrok-cli` at 9600-8-N-1
- scans for `0x4B` frame starts
- **brute-forces frame length and validates MODBUS CRC-16** on each candidate
- reports most-common bytes and looks for an incrementing sequence counter

```sh
./tools/ecm_capture.sh 30          # capture 30 s on CH0/CH1
./tools/ecm_analyse.py capture.sr  # validate frames
```

The CRC function is self-tested against the community frames (the table above), so a positive result
from the tool is meaningful.

⚠️ Two known gotchas for this toolchain: `sigrok-cli` needs an **absolute** path (its wrapper `cd`s into
its own bundle), and the flag is **`-P` / `--protocol-decoders`** (plural).

---

## What to do with a capture

1. **Confirm the channel toggles** — the raw edge check in `ecm_capture.sh`.
2. **Look for `0x4B` recurring** — if present, you're on the right channel and baud.
3. **Run the CRC validator.** ⭐ **A CRC-validated frame is proof you've cracked the framing.**
4. **Decode semantics** by correlating with `/poll` output — trigger an action, capture, poll, repeat.
5. The **sequence byte** should increment; that confirms the field boundaries. ⚠️ It is an **exchange
   counter shared by both directions** (the join key), it **wraps at 256**, and it **resets to `0x00`
   after a state-changing command** — so "it always goes up" is not a safe assumption (see the `seq`
   stall in `docs/COMMAND_MAP.md`).
6. ⚠️ **Capture `D0` and `D1` together.** A single line is ambiguous about direction. Capturing with the
   **module removed** is the strongest single test there is: whatever still talks is the ECM.

### If no `0x4B` and no CRC matches

| Symptom | Try |
|---|---|
| Channel never toggles | wrong pin — try CH1 (pin 4), or pin 5 |
| Toggles but no `0x4B` | wrong baud — sweep 19200 / 38400 / 57600 / 115200 |
| `0x4B` present, CRC fails | wrong frame length guess, or CRC covers a subset |
| All of the above | line may be **inverted** — add `:invert=1` to the decoder |

---

## Sources

- Home Assistant community forum thread on IntelliFire Wi-Fi modules, post 296 — the original capture
  and frame hypothesis
- [ECM_PORT_PINOUT.md](ECM_PORT_PINOUT.md) — this project's measurements of the same port
- `docs/PROTOCOL_DECODED.md` — the direction proof, the echo rule, and the module's write path
- `tools/ecm_analyse.py` — the validator (self-tested against the frames above)
