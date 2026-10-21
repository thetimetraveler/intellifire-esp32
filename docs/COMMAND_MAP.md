# ECM ↔ Wi-Fi module — command map (interim reference)

> **Capture-derived reference.** This document records what was observed on the wire. It does
> not claim to describe the ECM's internal firmware; a behavior that has not been seen in a
> capture is marked as a hypothesis.

> ### ⚠️ This is a HAND-WRITTEN interim reference, NOT machine-generated
>
> A future `tools/ecm_map_render.py` will generate the **authoritative** `COMMAND_MAP.json` from real
> sweep data. **This markdown is the hand-written stopgap** until then, and it can drift. Where the two
> disagree, the generated JSON wins.
>
> Nothing here is speculative filler: every row carries its **evidence count** and a
> **confidence** label. See the legend at the bottom.

---

## ⚠️⚠️ MAJOR CORRECTION — the direction is RESOLVED, and `0xE4`/`0xE1` are NOT commands to send

**This section is the most important part of this document. Earlier revisions of it implied an
implementation direction that is wrong.**

| Line | Captured on | Direction | Status |
|---|---|---|---|
| **D0** | ECM **pin 6** | **ECM → module** — the ECM transmits here | **confirmed** |
| **D1** | ECM **pin 4** | **module → ECM** — the module transmits here | **confirmed** |

**`0xE4` (flame) and `0xE1` (height) appear on D0 — so they are the ECM's ANNOUNCEMENTS of the state it
has adopted, not commands to transmit.** Earlier revisions called them "the commands", which reads as
though an implementer should *send* them. **That is wrong and must not be built on.**

### ⭐ The module never initiates a frame — it is a polled slave

The module has **no command frame of its own.** Its only write channel is a **4-byte data slot inside
its reply to the `BC` request**. An ESP32 that replaces the module must therefore **wait for a `BC`
request and answer it with the event payload in the data slot** — it cannot push a command out. Full
detail below.

### Why the direction is believed (three independent lines of evidence)

1. **The module-absent capture.** A community capture (post 296) was taken **on this same port with the
   module physically removed**, and its frames are byte-identical in shape to the D0 `BC` frames.
   **Frames that exist with no module fitted can only come from the ECM.** (The post-296 material is an
   **inherited claim** — but its frame format CRC-validates against the reference unit; see
   [FRAME_FORMAT.md](FRAME_FORMAT.md).)
2. **The `DC` status frame carries ECM-internal sensor knowledge** — pilot, flame height, and especially
   the byte-9 deadline flag, which the module has **no channel to learn**. It is on D0, so **D0 is the
   ECM**.
3. **Causality.** The new value appears **first on D1, 0.176 s before** the D0 announcement. The only
   party that *learned* the value is the module.

---

## What is actually established

**Two ECM state announcements (`0xE4`, `0xE1`), the module's write path that precedes them
(`0x67`/`0x64` in the `BC` reply slot), and four fireplace-state status fields** (pilot, height,
deadline flag, power), plus one likely length byte. That is the real result so far — **the purpose of the four routine
command bytes (`BC`, `DC`, `64`, `6E`) is still unknown.**

⭐ **Plus one more, added 2026-10-16: `0x20`, the ECM's POWER-ON / boot frame.** It is **not** a routine
command — it fires once per boot and appears nowhere else. See *"`0x20` — the POWER-ON frame"* below.

| # | Item | Value | Confidence | Evidence |
|---|---|---|---|---|
| 1 | **`0xE4` — flame ANNOUNCEMENT** (D0, ECM→module) | payload byte 2: `0x01` on / `0x00` off | **confirmed** | 3 observations, both transitions |
| 2 | **`0xE1` — flame height ANNOUNCEMENT** (D0, ECM→module) | payload byte 2 = height (internal 0–4) | **confirmed** | 4 observations — payload byte 2 tracked the app **monotonically** 3→2→1→0 |
| 3 | **Module-side opcode `0x67` = height write** (D1, in the `BC` reply slot) | slot = `67 00 <value> 00` | **confirmed** | **4/4** against the raw frames |
| 4 | **Module-side opcode `0x64` = flame write** (D1, in the `BC` reply slot) | slot = `64 00 <value> 00` | **strong** | two slot events (on **and** off); raw-frame check **1/1** |
| 5 | **The write path itself** — the `BC` reply data slot carries every write | slot = `<opcode> 00 <value> 00` | **confirmed** | **6/6 events**, gap 0.175–0.176 s |
| 6 | Status byte **7** = height | `0x00`–`0x04` | **confirmed** | tracked the commanded height exactly across a 4-step ladder; reports `0x04` while off (synthetic); the stored height is restored on flame-on |
| 7 | Status byte **5** = pilot | `0x01` on / `0x00` off | **confirmed** | 3 runs + idle (was "strong") |
| 8 | Status byte **10** = power (flame) | `0x01` on / `0x00` off | **confirmed** | 3 runs, both directions (was "strong") |
| 9 | Status byte **9** = 32-bit deadline-armed flag (not "hot") | `0x00`/`0x01` | **strong** | 3 runs; the "hot" label is unsupported |
| 10 | Status byte **1** = a length/count | `0x17` = 23 = `len(payload) − 2` | **likely** | holds for dev-`0x01` frames, **not** dev-`0x02` |
| 11 | **`0x0320`** (low byte `0x20`) — POWER-ON / boot frame (D0, ECM→module) | D0 payload `03 01 01`; D1 reply payload `03 01` (echo) | **confirmed** | **2 observations, byte-identical**, each the **first frame after a boot**, and **absent from every other capture** |

**Everything else — the *purpose* of the four routine command bytes (`BC`, `DC`, `64`, `6E`) — is
UNKNOWN.** Those are listed explicitly at the end rather than guessed at. (The **direction** is no
longer in that list: it is resolved, above. Nor is the **reply encoding**, which is now the echo rule
below.) **`0x20` is not in that list either** — it is identified as a boot frame, though **its payload is
not interpreted**.

---

## Bus basics (confirmed)

```
 4B | dev | seq | cmd | payload … | CRC16-LE        9600-8-N-1, lsb-first
```

| Field | Value | Confidence |
|---|---|---|
| Start byte | `0x4B` | confirmed |
| `dev` | `0x01`/`0x02` on D0; `0x03`/`0x04` on D1 (= D0 + 2) | confirmed |
| `seq` | **free-running per-exchange counter** — +1 per (D0,D1) pair, the **SAME value in both directions**; **wraps at 256** (`0xFF` → `0x00`); ⭐ **when a reply TIMES OUT the ECM resets it to `0x00` and HOLDS it at `0` while it keeps polling, then resumes counting from `1`**; ⭐ **and resets to `0x00` at POWER-UP** | confirmed |
| CRC | MODBUS CRC-16, poly `0xA001` reflected, init `0xFFFF`, over the frame **from `0x4B` inclusive**, stored **little-endian** | confirmed |

> ⚠️ **Corrected 2026-10-16:** the `seq` row used to read *"resets to `0x00` after a state-changing
> command"*. **That was an indirect correlation mistaken for the cause.** The reset is driven by a
> **timed-out reply**, not by the command — see *"⭐ The `seq` reset is caused by a TIMED-OUT REPLY"*
> below. A reader who saw the old text should read that section.

- ✅ **D0 (ECM→module) initiates; D1 (module→ECM) replies.** The request/response split is proven
  structurally, and the **polarity is now confirmed by content**, not just timing — see the correction
  at the top. (Earlier revisions said the polarity was "inferred from timing alone" and unconfirmed;
  that is superseded.)
- **The stock module replies almost instantly** — measured *end-of-request → start-of-reply* gaps of
  **41 µs (`BC`), 50 µs (`6E`), 114 µs (`DC`)**. ⭐ **That is how the stock hardware behaves — NOT a
  deadline the ECM enforces.** The real design budget is **~160 ms** (the ECM's next poll) and the ECM
  tolerates silence far beyond that. See the ⭐ timing section below; it is **not** a build blocker.
  (Corrected 2026-10-16 — this bullet used to call the figure "a build blocker".)

---

## ⭐ `0x20` — the POWER-ON frame, and it is reproducible

> ### ⚠️ `0x20` is a **BOOT** frame, not a command in the sense the others are
>
> It belongs in the command census, but it is **not** a member of the routine rotation
> (`BC`/`DC`/`64`/`6E`) and it is **not** an announcement of fireplace state. It fires **once per boot**.

Capture `captures/run-manual-20261016-112657/` (1 MHz, both channels, listen-only) was taken across a
**manual ECM power cycle**. The first frame after power-up:

```
t=57.070  D0  4B 01 00 20 03 01 01 DA 45     seq=0x00   cmd=0x20
t=57.080  D1  4B 03 00 20 03 01 8B 5A        seq=0x00
```

**The same frame appears a SECOND time in the same capture, byte-identical:**

```
t=12.589  D0  4B 01 00 20 03 01 01 DA 45     seq=0x00   cmd=0x20
t=12.598  D1  4B 03 00 20 03 01 8B 5A        seq=0x00
```

| Field | Value | Confidence |
|---|---|---|
| `cmd` | `0x20` = low byte of the **power-on / boot word `0x0320`** | **confirmed** |
| `dev` | `0x01` on D0, `0x03` on D1 (the usual +2 offset) | confirmed |
| `seq` | `0x00` — the **first** value after a reset | confirmed |
| D0 payload | `03 01 01` | observed — **meaning UNKNOWN, do not interpret it** |
| D1 reply payload | `03 01` | **echo** of the request's first two payload bytes (echo rule, below) |

> **Match the full 16-bit command word.** The boot frame's command word is `0x0320` (high byte `0x03`, low byte `0x20`). The schedule announcement `0x0220` shares the low byte and is **not** a boot frame, so a low-byte-only test is wrong.

**Why this is graded CONFIRMED** (2 observations, but decisive ones):

1. **Both occurrences are byte-identical** — same `cmd`, same `seq`, same payload, same CRC.
2. **Both are the FIRST frame after a boot**, and both sit at `seq=0x00`.
3. ⭐ **`0x20` appears in NO other capture** — checked across the three earlier captures
   (3,310 frames between them, whose censuses were `{BC, DC, 64, 6E}` plus a handful of `E1`/`E4`) and
   nowhere else. A command byte that appears **only at power-up and nowhere else** is a strong signal.

**The echo relationship is an observation only.** D1's reply payload is `03 01`, which **echoes the
request's first two payload bytes** — consistent with the project's verified reply-echo rule
(**642/642**). So the request content is `03 01` (echoed) plus one further byte, `01`.

> ⚠️ **Do NOT interpret what `03 01 01` means.** The echo relationship is observed; the *semantics* of
> the three payload bytes are not known. Recorded here as a correlation, not an interpretation.

⚠️ **A note on *who* is booting.** `0x20` transmits on **D0 = ECM→module**, so what the frame directly
evidences is that **the ECM emitted its power-on frame**. Whether the ECM resets *together with* the
module (the ECM supplies the module — `pin 3 = +5 V`), or emits `0x20` *in response to* a module boot,
is **not established by this capture. Do not state which.**

⭐ **Both occurrences are boots of the same module inside one capture — and one was spontaneous**
(t=12.589, no operator action; the other is the operator's power cycle at t=57.070, marked by an
**11.25 s silence on both lines**). The module's own `/poll` corroborates it: `uptime` stayed `0` with
`downtime` rising across both events. **This is a module reboot observed in the act** — full write-up in
[PROTOCOL_DECODED.md](PROTOCOL_DECODED.md), 2026-10-16.

### ⭐ `seq` starts at `0x00` at power-up — more evidence the ECM owns the counter

Immediately after the boot frame the normal rotation resumes, and the counter increments from zero:

```
t=57.070  seq=0x00  0x20        (boot frame)
t=57.239  seq=0x01  BC
t=57.811  seq=0x02  BC
t=58.160  seq=0x03  DC
t=58.576  seq=0x04  BC
```

So `seq` **resets to `0x00` at power-up** and counts up from there. ⚠️ **This is further evidence that
the exchange counter is owned by the ECM:** the ECM initiates every exchange and sets the sequence; the
module only answers with the same value.

> ⚠️ **A boot reset is NOT the same event as the timeout-driven park.** A boot reset is the ECM starting
> a fresh counter; a **park** is the ECM holding `0` *while it keeps polling* because a reply timed out
> (⭐ section below). Earlier text said the boot reset was *"consistent with the reset after a
> state-changing command"* — that link is withdrawn: the counter reset is caused by a **timed-out
> reply**, not by a command, and a boot is neither.

---

## ⭐ THE MODULE'S WRITE PATH — the module never initiates a frame

**This is the single most consequential section for anyone replacing the module.**

The module is a **polled slave**. Its only write channel is the **4-byte data slot inside its reply to
the `BC` request**:

```
D0  BC request:   4B 02 <seq> BC 02 04 <crc>
D1  idle reply:   4B 04 <seq> BC 02 04 00 00 00 00 <crc>
D1  height 3:     4B 04 <seq> BC 02 04 67 00 03 00 <crc>
D1  flame on:     4B 04 <seq> BC 02 04 64 00 01 00 <crc>
D1  pilot on:     4B 04 <seq> BC 02 04 65 00 01 00 <crc>
```

The 4-byte slot is `<opcode> 00 <value> 00`. **Idle replies fill it with zeros.**

(The leading `02 04` of the reply payload is **not** part of the slot — it is a **mechanical echo of the
request**, see the echo rule below.)

**Every commanded change in the set was preceded by exactly this pattern, 0.176 s before the D0
announcement, and the value matched — 9/9 commands across sessions:**

| D1 `BC` reply slot (on D1) | → | D0 announcement 0.176 s later |
|---|---|---|
| `02 04 67 00 03 00` | → | `0xE1` `00 01 03` |
| `02 04 67 00 02 00` | → | `0xE1` `00 01 02` |
| `02 04 67 00 01 00` | → | `0xE1` `00 01 01` |
| `02 04 67 00 00 00` | → | `0xE1` `00 01 00` |
| `02 04 67 00 04 00` | → | `0xE1` `00 01 04` |
| `02 04 64 00 01 00` | → | `0xE4` `00 01 01` |
| `02 04 64 00 00 00` | → | `0xE4` `00 01 00` |
| `02 04 65 00 01 00` | → | `0xDF` `00 01 01`  ← pilot |

**9/9 commands. Gap 0.175–0.176 s every time.** The **module-side opcode is `0x67` for height, `0x64`
for flame and `0x65` for pilot** — verified 4/4, 1/1 and 1/1 against the raw frames.

⭐ **Necessity as well as sufficiency.** Within one 300 s capture: of **391** D1 `0xBC` replies exactly
**3** carry a non-zero slot, and there are exactly **3** D0 announcements — 1:1, values matching. The
**388** idle slots produced **zero** announcements. So the ECM is not announcing from an internal timer:
**no write, no announcement.**

> ### ⚠️ `0x67`/`0x64`/`0x65` and `0xE1`/`0xE4`/`0xDF` are TWO UNRELATED NAMESPACES
>
> **Module slot opcodes (`0x67`, `0x64`, `0x65`) vs ECM announcement bytes (`0xE1`, `0xE4`, `0xDF`).**
> There is **no fixed transform between them — do not state one.** Flame is `0x64`→`0xE4` (+0x80) but
> pilot is `0x65`→`0xDF` and height is `0x67`→`0xE1` (both +0x7A). With three pairs no rule can be
> established. They are learned empirically, and the only thing tying them together is the observed
> 0.176 s correlation and the matching value.
>
> The same byte also appears in **both** namespaces: `0x64` is a *routine frame command byte* (D0
> `4B 01 <seq> 64 00 01 64`) **and** the flame *slot opcode*. Grepping "64" near an event finds the
> wrong frame first — there is a routine `0x64` frame ~13 s apart throughout.

> ### ⭐ Consequence for implementers
>
> **An ESP32 replacing the module cannot "send a command frame".** It must **wait for a `BC` request and
> answer it with the event payload in the data slot.**

### ⚠️ Correlated but UNINTERPRETED — the constant `00` in the slot

The slot shape `<opcode> 00 <value> 00` contains a **constant `00` between the opcode and the value**.
Its **meaning is unknown** — it is a *correlation*, not an interpretation. **Do not guess it.**

---

## ⭐ REPLY ECHO RULE — verified 642/642 pairs

**Every D1 reply echoes the FIRST TWO PAYLOAD BYTES of its request:**

| request | reply |
|---|---|
| `BC` `02 04` | `02 04 00 00 00 00` |
| `DC` `00 17 …` | `00 17` |
| `64` `00 01 64` | `00 01` |
| `6E` `00 01` | `00 01 00` |

Verified on **642 of 642 exchange pairs** in a live capture. So the `02 04` in a `BC` reply is a
**mechanical echo of the request, NOT a semantic header.**

> ⚠️ **If any text calls `02 04` a header or an opcode, that is wrong.** It is the request's first two
> payload bytes, repeated back.

**Likely explanation (HYPOTHESIS — not a finding):** the request's second byte is a **byte-count**,
i.e. "area `02`, up to `04` bytes", which fits the fixed 4-byte data slot exactly. Labelled a
hypothesis because it is a reasonable fit, not a proven field.

---

## ⭐ TIMING — three numbers, and only ONE of them is a deadline

> ### ⚠️ Corrected 2026-10-16 — this section was a "BUILD BLOCKER" and is not
>
> Earlier revisions of this section called the **41–114 µs** reply gap a **"BUILD BLOCKER"** and said
> *"ESPHome's default loop will not meet this"*, forcing bare-metal ESP-IDF. **That framing was wrong
> twice over:** it (a) treated *observed stock behaviour* as an *enforced deadline*, and (b) never
> carried the later finding that the ECM will wait **~5 s**. **The three numbers are separated below.**
> The measurement and the start-to-start vs end-to-start distinction are kept — both were correct.

| number | what it actually is | status |
|---|---|---|
| **41–114 µs** | how *fast the stock module* answers. **Observed behaviour of that hardware — NOT a requirement.** | measured |
| **~160 ms** | ⭐ **the real deadline.** End of one ECM frame → start of the next. The reply must be **on the wire inside that window** — *before the ECM sends its next request*. | **measured** |
| **~5 s** | how long the ECM will *wait* before parking its counter and accepting a backlog flush. **A tolerance, not a target.** | measured |

### ⭐ The real invariant — identical across three independent captures

> **End of one ECM frame → start of the next ECM frame:**
> **min 158.6–159.6 ms · p1 0.160 s · median 0.340 s**
> — identical across three independent captures (622 / 1237 / 1084 samples).

**The module's reply must be on the wire inside that ~160 ms window** — i.e. before the ECM sends its
next request. Reproducible from the per-frame timestamps in the capture frame lists
(`captures/*/frames_D0.jsonl`).

### The two earlier measurements (both kept — they are correct)

| Measurement | Basis | Value |
|---|---|---|
| **end-of-request → start-of-reply** | direct, per frame type | **41 µs (`BC`)**, **50 µs (`6E`)**, **114 µs (`DC`)** |
| *start-to-start* (the frequently-quoted figure) | D0 start vs D1 start | 8.374 ms / 8.383 ms / 32.406 ms — i.e. **≈ one D0 frame duration, ratio ~1.005** |

⚠️ **These are two different measurements of the same back-to-back exchange — do not conflate them.**
The *start-to-start* figure includes the whole request frame (a byte is 10 bit-periods at 9600-8-N-1:
8 B ≈ 8.333 ms, 31 B ≈ 32.292 ms), so it *looks* like "one frame duration". The **end-to-start** figure
describes the stock responder; the **~160 ms inter-frame window** is what an implementer must beat.

### ⭐ Consequence for implementers — ESPHome IS viable

**41–114 µs is *observed stock behaviour*, not a deadline.** Against the real **~160 ms** budget,
**ESPHome's default loop has roughly 10× headroom** — so **ESPHome is viable and bare-metal ESP-IDF is
NOT forced by timing.** (Interrupt-driven RX, end-of-frame detection and precomputed replies remain
good practice, not a requirement.)

⚠️ **Caveat, still open:** the ESPHome loop's actual latency **has not yet been measured by us
on-device**. The headroom argument is arithmetic against the measured ~160 ms figure; it is not a bench
measurement of an ESPHome responder.

---

## ECM state announcements — CONFIRMED

> **Renamed.** Earlier revisions headed this "Commands — CONFIRMED" and described the two frames as
> things to send. They are **D0 frames: the ECM announcing its state to the module.** The *module-side*
> writes that precede them are in the write-path section above.

### `0xE4` — flame ANNOUNCEMENT

```
FLAME ON   t=95.671   D0  4B 01 9D E4 00 01 01 FA A9     payload 00 01 01
FLAME ON   t=109.540  D0  4B 01 BE E4 00 01 01 3F 6E     payload 00 01 01
FLAME OFF  t=35.393   D0  4B 01 EB E4 00 01 00 F2 A2     payload 00 01 00
```

| Field | Meaning | Confidence |
|---|---|---|
| payload byte 2 | `0x01` = flame **on**, `0x00` = flame **off** | **confirmed** (3 observations, both transitions) |

Ten payload bytes 0 and 1 (`00 01`) are constant across all three; their meaning is unknown.

**Reply:** shortened, carries no value byte —

```
t=95.680  D1  4B 03 9D E4 00 01 E5 FB        <- same seq=0x9D as the flame-on above
```

Shape: `4B 03 <seq> E4 00 01 <crc>` — and note the reply payload `00 01` is exactly the **echo** of the
request payload's first two bytes (echo rule above).

### `0xE1` — flame height ANNOUNCEMENT

An operator stepped the app's flame height **down one step at a time** while a capture rolled. Both the
announced payload and the status byte tracked it **monotonically and exactly**:

```
t=20.457  D0/0x01/0xE1  payload=00 01 03   ->   status byte 7 = 0x03
t=33.614  D0/0x01/0xE1  payload=00 01 02   ->   status byte 7 = 0x02
t=40.681  D0/0x01/0xE1  payload=00 01 01   ->   status byte 7 = 0x01
t=51.112  D0/0x01/0xE1  payload=00 01 00   ->   status byte 7 = 0x00
```

Earlier evidence (the first `0xE1` frame captured):

```
t=91.81   D0  4B 01 D3 E1 00 01 04 52 68     payload 00 01 04
```

| Field | Meaning | Confidence |
|---|---|---|
| payload byte 2 | the height, **internal 0-based** (`0x00`–`0x04`) | **confirmed** |

**This is the difference between CORRELATED and ENCODED.** A single payload value would be consistent
with a constant; **four values tracking the height exactly proves payload byte 2 carries the value.**
`0xE1` is upgraded from "correlated / single observation" to **CONFIRMED, 4 observations**. **Same
`00 01 <value>` payload shape as `0xE4`.**

**Reply:** shortened, carries no value byte — `D1/0x03/0xE1`, payload `00 01`.

#### ⭐ Flame-off reports the maximum height, with no announcement

In the same capture the flame was turned off at t=72.58 (`D0/0x01/0xE4`, payload `00 01 00`).
**Status byte 7 jumped `0x00` → `0x04` at t=73.41 with NO `0xE1` frame anywhere near it.** The
status frame reports a **synthetic maximum (4) while the burner is off**; the ECM's stored height is
restored on flame-on. **This explains a previously unexplained pattern: every idle capture showed byte
7 = `0x04`** — that is the reported value while off, not the stored height.

> ### ⚠️ Height can only be read or set while the burner is RUNNING
>
> A height change commanded with the flame **off** produced **no bus traffic at all** — not an ignored
> command, but **no frame on the wire**. The status value while off is synthetic (maximum); the stored
> height is restored on flame-on. Height is therefore only meaningful during a burn.

> ### ⚠️ Height is 0-based internally, 1-based in the app
>
> `/poll` reports `height` **0–4**. The Home Assistant / app control is **1–5**.
>
> | App height | `/poll` height | status byte 7 | module slot value |
> |---|---|---|---|
> | 1 | 0 | `0x00` | `0x00` |
> | 5 | 4 | `0x04` | `0x04` |
>
> This was once written up as **"cloud staleness"** between HA and `/poll`. **It is not staleness — it is
> an off-by-one between two representations.** Any "height" read from `/poll` or a status byte is the
> *internal* value.

### ⭐⭐ `0xDF` — pilot ANNOUNCEMENT, and the write→announce chain pinned (2026-10-16)

**`0xDF` was missing from this document until now.** It is the third member of the announcement family,
same `00 01 <value>` shape, and it is the one that gates ignition:

```
t=130.881  D0  4B 01 86 DF 00 01 01 93 8F     payload 00 01 01   pilot ON
t=130.891  D1  4B 03 86 DF 00 01 92 D2        shortened reply, no value byte
```

⭐ **The full causal chain was captured end-to-end.** The app writes into the module's
`0xBC` reply slot; the ECM announces it **one exchange later** — write at `seq N`, announcement at
`seq N+1`:

| app's write (D1 `0xBC` slot) | ECM announcement (D0) | Δ | meaning |
|---|---|---|---|
| seq `0x85` `65 00 01 00` | seq `0x86` `DF 00 01 01` | **0.176 s** | pilot **on** |
| seq `0x25` `64 00 01 00` | seq `0x26` `E4 00 01 01` | **0.176 s** | flame **on** |
| seq `0x2B` `67 00 00 00` | seq `0x2C` `E1 00 01 00` | **0.176 s** | height **0** |

**Necessity and sufficiency both hold inside the capture:** of **391** D1 `0xBC` replies exactly **3**
carry a non-zero slot, and there are exactly **3** D0 announcements — 1:1, values matching. The **388**
idle slots produced **zero** announcements, so the ECM is not announcing from an internal timer. The Δ
is **pinned at 0.176 s** (spread ~120 µs) against inter-frame spacing varying 160–610 ms — a fixed
processing latency, not coincidence. Now **9/9 across sessions**.

⚠️ **Load-bearing premise:** this assumes there is **no out-of-band app→ECM path** (a cloud→RF bridge to
an ECM receiver). The capture cannot rule that out. A **wall-switch** ignition is the test — it should
produce an announcement with **no** slot write.

⚠️ **Namespace trap.** `0x64`/`0x65`/`0x67` are *slot opcodes* (module → ECM, inside the `0xBC` reply);
`0x64`/`0x6E`/`0xE4`/`0xE1`/`0xDF` are *frame command bytes*. The same byte appears in both, unrelated —
grepping near an event finds the wrong frame first. There is **no constant offset** between the two
namespaces (flame `0x64`→`0xE4` is +0x80, but pilot `0x65`→`0xDF` and height `0x67`→`0xE1` are +0x7A).

### ⚠️⚠️ `status byte 7 == 4` does NOT mean "flame off"

An earlier reading held that height `4` is a flame-off sentinel which lighting drops to `0`. **Wrong** —
the burner can be lit while byte 7 still reads `4`:

```
t=150.715  pilot=01  height=04  power=00
t=151.903  pilot=01  height=04  power=01   <-- BURNING, height still 4
t=153.021  pilot=01  height=04  power=01   <-- BURNING, height still 4
t=154.265  pilot=01  height=00  power=01   <-- only now, after the 0xE1 write
```

`4` is the **stored default restored at flame-off**, and it persists **through ignition** until a height
write lands. The `4 -> 0` seen here was caused by the app's height write 1.4 s later — **not** by
flame-on. Use status byte **10** (flame) to decide "burning", never byte 7.

---

## Status frame `D0/0x01/0xDC` — the state report

Payload indices, **0-based**. Idle payload is `00 17 00 00 00 00 00 04` + 17 zeros.

| index | value | field | confidence | evidence |
|---|---|---|---|---|
| 1 | `0x17` = 23 = `len(payload) − 2` | length/count | likely | holds for dev-`0x01` frames; **not** dev-`0x02` |
| 5 | `0x01` / `0x00` | `/poll` `pilot` | **confirmed** | 3 runs + idle |
| **7** | `0x00`–`0x04` | `/poll` `height` (internal) | **confirmed** | 4-step ladder 3→2→1→0 tracked exactly; reports `0x04` while off (synthetic); stored height restored on flame-on |
| 9 | `0x00` / `0x01` | 32-bit deadline-armed flag (not `/poll` `hot`) | strong | 3 runs; the "hot" label is unsupported |
| 10 | `0x00` / `0x01` | `/poll` `power` (flame on/off) | **confirmed** | 3 runs, both directions |

**25 payload bytes total** (the 31-byte frame is 4 header + 25 payload + 2 CRC; the first two payload
bytes are the command-high byte and the length `0x17`, leaving 23 status data bytes). Five fields are
mapped, the rest are unmapped. Bytes 5 and 10 are genuinely distinct fields (pilot stayed `0x01` while
power was `0x00`).

**This frame is on D0**, which is exactly why it is evidence for the direction: it reports
ECM-internal sensor knowledge the module could not otherwise have (see the correction at the top).

---

## The four routine frames — purpose UNKNOWN

All four **D0 payloads** are **CONSTANT while idle**. They dominate the bus — `BC` alone is roughly
**50% of all frames**.

| key | payload | total len | D1 reply len |
|---|---|---|---|
| `D0/0x02/0xBC` | `02 04` | 8 | 12 |
| `D0/0x01/0xDC` | `00 17 00 00 00 00 00 04` + 17 zeros | 31 | 8 |
| `D0/0x01/0x64` | `00 01 64` | 9 | 8 |
| `D0/0x02/0x6E` | `00 01` | 8 | 9 |

⚠️ **The *purpose* of none of these four command bytes is known. Do not guess.**

**One thing *is* known about `BC`, and it matters:** the **`BC` reply is the module's write channel**
(above). So `BC` is *confirmed* as the carrier of every module-side write, even though **what the `BC`
command byte means to the ECM is still unknown.** Do not upgrade that into a full interpretation.

Note the asymmetry: the `DC` frame's **payload** is mapped (it is the state report) but the `DC`
**command's role** is not. Likewise `DC`/`64`/`6E` are entirely uninterpreted as commands.

> ⚠️ These are the **D0** payloads. The D1 `BC` **reply** is *not* constant — it is where the module's
> write slot varies (see the write-path section).

### ⭐ A fifth frame byte, since identified — `0x20`, the POWER-ON / boot frame

`0x20` is **not** a routine frame: it fires **once per boot** and appears in no other capture. It is
listed here so the census is complete and so it is not mistaken for a fifth routine command.

| key | payload | total len | D1 reply len | status |
|---|---|---|---|---|
| `D0/0x01/0x20` | `03 01 01` | 9 | 8 | ⭐ **newly identified — the POWER-ON / boot frame** (2 observations, byte-identical; payload **not** interpreted) |

⚠️ **The four routine frames above are still the only unidentified *routine* frames.** `0x20` moved out
of that column, not into it: its **role** (boot) is identified; its **payload** is not. See
*"`0x20` — the POWER-ON frame"* above.

---

## ✅ RESOLVED — the D1 reply to `BC` is the module's write channel

> **Updated.** This section used to be headed "⚠️ Correlated but UNINTERPRETED" and described the extra
> bytes in the D1 reply as unexplained. **They are no longer uninterpreted.** The extra bytes *are* the
> module's 4-byte write slot, and the change is the value it is writing.

At a flame transition, the D1 reply to the *preceding* `BC` frame carries extra bytes:

| moment | D1 reply payload to `BC` | read as |
|---|---|---|
| idle | `02 04 00 00 00 00` | echo `02 04` + **zeroed slot** |
| flame **on** | `02 04 64 00 01 00` | echo `02 04` + slot `64 00 01 00` → opcode `0x64`, value `0x01` |
| flame **off** | `02 04 64 00 00 00` | echo `02 04` + slot `64 00 00 00` → opcode `0x64`, value `0x00` |

**What this used to be recorded as:** a correlation of unknown meaning, "in particular, whether
`0x64` here is the same thing as the `0x64` *command byte*". **It is the module's write opcode — it is
not the `0x64` command byte**, which is a separate, still-uninterpreted routine command (see the
namespace warning above).

---

## ⭐ The `seq` reset is caused by a TIMED-OUT REPLY — not by the command

> ### ⚠️ Corrected 2026-10-16 — the cause is now known, and it is not what this section used to say
>
> This section was headed *"OPEN OBSERVATION — the `seq` stall after a state change"*, said the counter
> *"resets to `00` after a state-changing command"*, and left the **cause UNKNOWN**. **That was an
> indirect correlation mistaken for the cause.** A capture shows the actual mechanism frame-by-frame.

**`seq` is a free-running per-exchange counter.** What resets it is **a reply that times out** — not the
command: when a reply does not arrive, the ECM **resets the counter to `0` and HOLDS it at `0`** while it
keeps polling; when the module recovers, the ECM **resumes counting from `1`** — *not* from where it left
off.

Verified frame-by-frame across a stall in `captures/run-manual-20261016-112657/`:

```
70.400  D0  DC  seq=0x24      -> module is silent; no reply
70.817 .. 74.737   D0 polls 11x, EVERY ONE at seq=0    <- ECM parks the counter at 0
74.889  D1  DC  seq=0x24                                <- module answers the 4.5 s-old request
74.897 .. 75.004   D1 flushes 11 replies, ALL at seq=0
76.027  D0  seq=0x01                                    <- ECM resumes counting from 1
```

⭐ **The module's "backlog flush" is NOT many different sequence values:** every flushed reply carries
`seq=0`, **because every parked request did.** The single `seq=0x24` reply at 74.889 is the late answer
to the one request outstanding when the module went silent.

### ⚠️ Consequence — the link SELF-RECOVERS, with no resynchronisation logic required

**This is important and was previously unstated anywhere in these docs:**

⭐ **A missed reply, a dropped frame, an ESP32 reboot, or an over-the-air update all land the ECM in a
state it leaves on its own.** The ECM parks its counter, keeps polling, and resumes counting from `1`
when replies return.

> ⚠️ **Any replacement design must therefore NEVER depend on `seq` continuity.** Treat `seq` as an
> opaque per-exchange value to **echo back** — never one to predict, count, or resynchronise.

### ⚠️ The silence is in the MODULE's transmit path — the command does not cause it

**The ECM polls happily throughout and simply waits.** The stall is in the **module's transmit path**;
it is **not** the command acting on the ECM's counter. A replacement board therefore **neither inherits
nor needs to emulate it.**

**The observations below are correct as data** — the stall follows command execution, fires across two
command bytes, and is independent of `/poll` load. What changes is the *interpretation*: the counter
reset is a **reply-timeout response**, not a command effect.

**Earlier observations (4):**

| command | cmd at | stall at | delta |
|---|---|---|---|
| flame ON `0xE4` | 95.671 | 97.34 | 1.67 s |
| flame ON `0xE4` | 109.540 | 111.00 | 1.46 s |
| flame OFF `0xE4` | 35.393 | 36.894 | 1.50 s |
| height `0xE1` | 91.813 | 92.848 | 1.04 s |

**Last capture — measured after EVERY command (5), Δ 1.3–1.7 s:**

| command | payload | delta to stall |
|---|---|---|
| `0xE1` | `00 01 03` | +1.63 s |
| `0xE1` | `00 01 02` | +1.33 s |
| `0xE1` | `00 01 01` | +1.72 s |
| `0xE1` | `00 01 00` | +1.61 s |
| `0xE4` | `00 01 00` | +1.46 s |

All frames involved are CRC-valid. Combined with the earlier runs this is **9 observations across two
command bytes**.

- A `/poll` gap (15.8–20.1 s) is logged around the same moment.
- **Independent of `/poll` load** — reproduced in runs whose poll pressure differed by ~100×.
- **Fires for the height command as well as the flame command**, which is why it is described as
  **a write-correlated stall in the module's transmit path** and **almost certainly benign** —
  **NOT a defect**. (Framed 2026-10-16: previously *"systematic behaviour when a write is applied"* —
  the cause is the reply timeout, above, not the write acting on the counter.)

⚠️ **Which side?** Now answerable: the **silence** is in the **module's transmit path** (the ECM polls
throughout), and the **counter parking** is **ECM-side response to the timeout**. So a replacement board
would **neither inherit nor need to emulate** the stall. (This paragraph previously read *"not yet known
whether this is module-side or ECM-side"* — superseded by the frame-by-frame sequence above.)

⚠️ **Do NOT present this as a defect.** The **null test** was
proposed to name a cause; the cause is now known from the sequence above (a timed-out reply), so the
null test is no longer the blocker. Note that a "set height to its current value" null remains **hard to
stage**, because setting the same value may not transmit at all.

#### ⭐ New (2026-10-16): a stall with NO command at all — and it is the SAME mechanism

**One occurrence, 1/1. ⭐ Cause now known: a MISSED REPLY.** This is **the same event** that established
the timeout mechanism above — it is what a reply timeout looks like when no command precedes it.

In `captures/run-manual-20261016-112657/` the module's transmit line went silent for **4.814 s**:

| Element | Value |
|---|---|
| Last D1 frame before silence | `BC` reply at **t=70.075** |
| D1 resumes | **t=74.889**, with `DC` at **seq=36** — answering the outstanding D0 exchange |
| Nearest preceding D0 frames | `0x6E` t=69.583 · `0x64` t=69.746 · `BC` t=70.051 · `DC` t=70.400 — **all routine** |
| `0xE1` / `0xE4` in this capture | **zero** — the capture contains no flame or height traffic at all |

This is the **same shape** as the post-command stall above (the counter parks at `00`), but **no `0xE1`
or `0xE4` preceded it.** During the silence D0 kept polling normally at its usual ~0.3–0.6 s spacing,
**with `seq` parked at `0x00`** from t=70.817 to t=74.737.

⭐ **Both earlier candidate readings resolve cleanly against the mechanism above:**

1. **"A trigger besides state-changing commands"** — ✅ **met: a missed reply.** The `seq=0x24` `DC`
   request at t=70.400 went unanswered, the ECM timed out, parked the counter at `0`, and the module
   answered 4.5 s late.
2. **"The module was still settling ~13 s after its boot"** — still open as the **reason the reply was
   missed**, but it is **not** the mechanism of the reset. The reset mechanism is the timeout.

It is **not** a defect.

---

## Open questions

- **What `BC`, the `DC` *command role*, `0x64` and `0x6E` mean.** (The `BC` *reply* is now known to be
  the module's write channel; the command bytes' purposes are not.)
- **How long the ECM waits before it parks its counter, and whether a *late slot write* is still
  honoured.** The shape is now known (a timed-out reply parks the counter at `0`; the ECM keeps polling
  and resyncs from `1`), but the timeout value, and whether the ECM still *acts on* a write that arrives
  late, are not. ⚠️ The ECM tolerating late *replies* is a different code path from acting on a late
  *command*.
- **Whether the ECM *enforces* the ~160 ms window** — or merely happens to receive replies inside it.
  ⚠️ **Partly answered:** the ECM demonstrably tolerates **~5 s** of silence and accepts a backlog flush,
  so **~160 ms is the design budget, not a hard enforced deadline** — a replacement has headroom.
- **Whether the announcements also appear for changes initiated by the wall switch or the RF remote.**
  Nobody has captured that — and it would **cleanly confirm the ECM-origin reading**, since a
  wall-switch change originates at the ECM and not in the app.
- The constant `00` inside the write slot (`<opcode> 00 <value> 00`) — **correlated, uninterpreted.**
- Whether the **module's transmit-path stall** (the ~5 s silence itself, *not* the counter reset) is
  benign — the reset's cause is known, but why a write makes the module's transmit path go quiet is not.
- **What `0x20`'s payload `03 01 01` means.** Its **role** (boot) is identified; its **payload** is not.
  **Do not guess it.**
- **Whether the ECM resets *with* the module, or emits `0x20` *in response to* a module boot.** The frame
  is on D0 (ECM), so the capture shows only that the ECM emitted it. Unknown.
- **Whether `0x20` is emitted at *every* boot.** Two observations, both at a boot, zero counterexamples —
  but only two. A third capture that reaches a boot without a `0x20` would refute it.
- **Why the reply at t=70.400 was missed** (the 4.814 s stall with no preceding command) — ⭐ the *reset
  mechanism* is known (a timed-out reply), but the reason that one reply never came is not.

**Resolved and no longer open:** which D0/D1 line is which direction (**D0 = ECM→module,
D1 = module→ECM**); the reply encoding (**the echo rule**); the D1 `BC` anomaly (**the module's write
channel**); the role of `0x20` (**the ECM's power-on / boot frame**); whether `seq` restarts at boot
(**yes — `0x00`, evidence the ECM owns the counter**); ⭐ **what causes the `seq` reset** (**a timed-out
reply** — not the command — and the link **self-recovers** with no resynchronisation logic).

---

## How these were obtained

| Element | Detail |
|---|---|
| **Harness** | `tools/ecm_harness.py` — **listen-only**; it transmits nothing. The bus is driven only by the fireplace's own control path (Home Assistant / the app) |
| **Capture** | 1 MHz logic-analyser capture on **ECM pins 4 and 6**, **GND on pin 2/8** |
| **Read-path answer key** | `GET /poll` on the module, polled alongside each capture, tying every changed frame to a real fireplace value |
| **Real ignition** | The **fireplace was actually ignited** to obtain the flame and height data — live gas-appliance evidence, not simulation |
| **Driving the appliance** | The **operator drives the app while a long capture rolls** — human pacing, no cloud-latency guesswork, and the long window absorbs any delay. The stock Home Assistant integration may be configured in cloud mode, so HA-issued commands can take tens of seconds to reach the module |
| **Boot capture** | `captures/run-manual-20261016-112657/` — 1 MHz, both channels, **listen-only**, taken **across a manual ECM power cycle**. The operator drove the **power cycle by hand while a long capture rolled** — the **reliable method** (paced background timers repeatedly failed). ⚠️ The harness flagged this run `CAPTURE_SHORT` (427.8 s of a planned 900 s), but **every frame that is present is CRC-valid** (ratio 1.0) and both boot events fall inside the captured window. |
| **Read path vs. serial link** | ⚠️ **They can fail independently.** This run needed a new `--allow-no-poll` flag because **`/poll` was down while the serial link was perfectly healthy** — without the flag the harness aborts (it treats the read path as the answer key). |
| **Correlation method** | The harness fires the mark at a known **offset into** the capture and finds the change by **content** (the first frame differing from baseline), never by timestamp |
| **Write-path correlation** | The slot value in the D1 `BC` reply is matched to the D0 announcement **0.176 s later, by value** — 6/6 commands |
| **Echo rule** | Counted across **642/642 exchange pairs** in a live capture |

Full structural write-up: **`docs/PROTOCOL_DECODED.md`**. Frame-format proof: **`docs/FRAME_FORMAT.md`**.

---

## ⚠️ SAFETY

**`/poll` is the READ path and is safe. `/post` is the WRITE path and must NEVER be called in a read-only
context** — every field it can change drives the actual fireplace.

Analysis is **listen-only**. Analyzer ground connects **first** and is removed **last**. **Never clip the
analyzer to ECM pin 3 (+5 V supply).** Keep the RF remote's batteries out during a session. Mains is live
in the cavity — ECM low-voltage side only.

---

## Confidence legend

| Label | Means |
|---|---|
| **confirmed** | Repeated observations covering the full range, or a mechanism proven mathematically. Safe to build on. |
| **strong** | Several consistent observations, one plausible alternative not excluded. |
| **probable** | Multiple observations, but the field was not deliberately varied in isolation. |
| **likely** | Consistent with every observation so far, but few of them. |
| **hypothesis** | A guess consistent with the data. **Not** a finding. |
| **correlated** | A relationship was observed; the **mechanism is unknown**. |

**One observation is never "confirmed".** Where a claim rests on a single capture, the row says so.
