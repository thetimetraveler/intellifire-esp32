# Protocol decoded — ECM ↔ WFM serial link

**Date:** 2026-10-16. **Source:** `logic_analyzer_data/dump1.sr` (the initial capture), superseded by
`logic_analyzer_data/dump3.vcd` (1 MHz).

**This document records a working decode of this link.** The Home Assistant community thread's two attempts
(2022–23) both
failed; the question was still open in July 2026.

> ## ⚠️⚠️ READ FIRST — THE DIRECTION IS RESOLVED, and it reverses the obvious implementation
>
> | Line | Captured on | Direction | Status |
> |---|---|---|---|
> | **D0** | ECM **pin 6** | **ECM → module** — the ECM transmits here | **confirmed** |
> | **D1** | ECM **pin 4** | **module → ECM** — the module transmits here | **confirmed** |
>
> **`0xE4` (flame) and `0xE1` (height) appear on D0 — so they are the ECM's ANNOUNCEMENTS, not the
> commands to send.** Earlier revisions of this document headed them "COMMANDS" and called them "the
> two decoded commands". **That framing implied an implementer should transmit them. It is wrong.**
>
> **The module never initiates a frame. It is a POLLED SLAVE.** Its only write channel is a **4-byte
> data slot inside its reply to the `BC` request** — a `0xE4`/`0xE1`-shaped *effect* is written as
> `67 00 <value> 00` / `64 00 <value> 00` on **D1**, and the ECM then announces the result on D0
> **0.176 s later**. See *"The module's write path"* below.
>
> **Three independent lines of evidence:**
>
> 1. **The module was physically REMOVED** for the post-296 capture on this port, and its frames are
>    byte-identical in shape to the D0 `BC` frames. **Frames that exist with no module fitted can only
>    come from the ECM.** (post-296 is an **inherited claim**, but its frame format CRC-validates on our
>    own unit — see `docs/FRAME_FORMAT.md`.)
> 2. **The `DC` status frame carries ECM-internal sensor knowledge** — pilot, flame height, and
>    especially the `hot` thermal state, which the module has **no channel to learn**. It is on D0, so
>    **D0 is the ECM**.
> 3. **Causality:** the new value appears **first on D1, 0.176 s before** the D0 announcement. The only
>    party that *learned* the value is the module.
>
> **This supersedes the earlier position** that the polarity was "inferred from timing alone and
> UNCONFIRMED". It is now confirmed **by content**.

---

> ### ⚠️ READ FIRST — dump1 (20 kHz) is UNRELIABLE; dump3 is authoritative
>
> **`dump1` must not be used for ANY structural claim.** At 20 kHz with 9600 baud there are only ~2.08
> samples per bit, which corrupts the decode. Concretely, `sig_dump1_D1.txt` decodes device bytes as
> `0x84`, `0x83`, `0xF0` — but device ids on D1 must be `0x03`/`0x04`. **Those are bit errors**, and
> dump1's D1 stream in particular is corrupt.
>
> **`dump3` (1 MHz, 104 samples/bit) is the authoritative capture.** It decodes every frame cleanly with
> sigrok's standard decoder and no phase tuning.
>
> **Two claims in this document were corrected because they came from dump1:** the `seq` semantics
> (corrected to an exchange counter) and the D1 framing (corrected to the reply direction). The 46-frame
> CRC count derived from dump1 is an early result and is **superseded/unreliable**.

---

## ⚠️ Capture settings — both wrong, both fixable

From the PulseView session file and the `.sr` metadata:

| Setting | Was | Should be |
|---|---|---|
| **Sample rate** | **20 kHz** | **≥ 250 kHz**, ideally **1 MHz** |
| **UART bit order** | **`msb-first`** | **`lsb-first`** |
| Baud rate | 9600 ✅ | 9600 ✅ |
| Channels | D0, D1 | ✅ |

**20 kHz gives only ~2.08 samples per bit** at 9600 baud (UART receivers want 8–16).

**The data is NOT trustworthy at this rate.** Sweeping the decode phase found **phase 0.25** yields 46
CRC-valid frames where phase 0.0/0.5/0.75 yield ~0. That phase sensitivity is a direct artefact of the
low sample rate — **recapturing faster makes it disappear** — and it means the phase-0.25 decode is only
*one* of several possible readings, not a recovered truth. **`dump1` must not be used for any structural
claim** (see the ⚠️ warning above); `dump3` at 1 MHz replaced it and needs no phase tuning at all.

**`msb-first` is definitively wrong:** lsb-first yields 26+ `0x4B` start bytes; msb-first yields **zero**.

---

## ✅ The community capture (post 296) is now fully confirmed with independent data

The hypothesis was validated three ways:

1. **CRC-16 MODBUS, little-endian** — both of the community capture's published sample frames validate
   mathematically
   (see `FRAME_FORMAT.md`).
2. **`0x4B` start byte** — appears 26–47 times in the reference capture; **zero** under msb-first.
3. **The status payload matches byte-for-byte.** The community capture saw
   `4B 01 9B DC 00 17 00 00 00 00 00 04 …`;
   we captured `4B 01 2A DC 00 17 00 00 00 00 00 04 …`.

**46 CRC-validated frames.** ⚠️ *Superseded — this is an early dump1 (20 kHz) result and is unreliable;
`dump3` at 1 MHz is the authoritative count. See the warning above.* With ~2,800 CRC trials, chance false
positives would be ~0.04 — so the frames that *did* validate are real, but the count itself is not
trustworthy.

**⭐ The community capture also carries a direction fact that was not available before:** it was taken
on this port
**with the module removed**. Frames that exist with no module fitted can only come from the ECM (see the
read-first block above).

---

## Frame structure

```
 4B | dev | seq | cmd | payload … | CRC16-LE
```

| Field | Notes |
|---|---|
| Start | **`0x4B`** |
| **Device** | `0x01`/`0x02` on D0; `0x03`/`0x04` on D1 |
| **Sequence** | ⭐ **an EXCHANGE counter, +1 per (D0,D1) exchange PAIR — D0 and D1 share the same value, making it the JOIN KEY between the two directions** |
| Command | `0xBC`, `0xDC`, `0x64`, `0x6E` (routine, purpose unknown); ⭐ **`0xE4` = flame announcement, `0xE1` = flame height announcement** (decoded — see below); ⭐ **`0x20` = the ECM's POWER-ON / boot frame** (decoded — see below). ⚠️ The `0xE4`/`0xE1`/`0x20` frames are **D0/ECM frames**, not commands for an implementer to send |
| Payload | 2–25 bytes |
| CRC | **MODBUS CRC-16, little-endian** — poly `0xA001` (reflected), init `0xFFFF`, computed over the frame **from the `0x4B` start byte inclusive** through the last payload byte, stored **little-endian** |

**Serial parameters: 9600-8-N-1, lsb-first.**
**Direction: D0 (pin 6) = ECM→module. D1 (pin 4) = module→ECM.** — confirmed by content, above.

### ⭐ The sequence number is an EXCHANGE counter (corrected)

> **Corrected:** this section previously read "*a single GLOBAL counter, +1 per frame, shared across
> devices*", showing a run `29 2A 2B 2C … 46`. That was wrong. The +1-per-frame reading was an artefact
> of the corrupt 20 kHz `dump1` decode (see the ⚠️ warning at the top). The 1 MHz capture disproves it.

From `dec3_D0.txt` / `dec3_D1.txt` (and `sig_dump3_D0.txt` / `sig_dump3_D1.txt`):

```
D0 first frame:  4B 02 98 BC 02 04 98 47              -> dev=0x02 seq=0x98 cmd=0xBC
D1 first frame:  4B 04 98 BC 02 04 00 00 00 00 B4 D5  -> dev=0x04 seq=0x98 cmd=0xBC
```

**BOTH directions carry `seq=0x98`.** The same value appears **twice**, not `98` then `99`. The next
exchange is `0x99` on both directions, then `0x9A` on both, then `0x9B` on both.

**`seq` increments by +1 per (D0,D1) exchange PAIR, and D0 and D1 share the same `seq` value.** It is
therefore the **JOIN KEY between the two directions: a D0 frame with `seq=N` is answered by the D1 frame
with the same `seq=N`.**

⚠️ **`seq` WRAPS AT 256** — `0xFF` is followed by `0x00`. A consumer must treat it as an **8-bit wrapping
counter**, not a monotonic one.

⭐ **`seq` is a FREE-RUNNING counter — and when a reply TIMES OUT the ECM resets it to `0x00` and HOLDS it
at `0` while it keeps polling, then resumes counting from `1`.**

> ⚠️ **Corrected 2026-10-16:** this read *"`seq` also RESETS to `0x00` after a state-changing command"*.
> **That was an indirect correlation mistaken for the cause.** The reset is a **reply-timeout response**,
> not a command effect — see *"⭐ The `seq` reset is caused by a TIMED-OUT reply"* below.

⭐ **`seq` ALSO RESETS to `0x00` at POWER-UP.** A capture taken across a manual power cycle shows the
counter restarting from zero on the boot frame and counting up from there (full sequence below). That
the counter restarts at boot — as well as after a timed-out reply — is **further evidence that the
exchange counter is owned by the ECM:** the ECM initiates every exchange and sets the sequence; the
module only answers with the same value.

---

## ⭐⭐ The module's write path — the module never initiates a frame

**The single most consequential fact in this document for anyone replacing the module.**

**The module is a polled slave.** It does not send command frames of its own. **Its only write channel is
a 4-byte data slot inside its reply to the `BC` request:**

```
D0  BC request:   4B 02 <seq> BC 02 04 <crc>
D1  idle reply:   4B 04 <seq> BC 02 04 00 00 00 00 <crc>
D1  height 3:     4B 04 <seq> BC 02 04 67 00 03 00 <crc>
D1  flame on:     4B 04 <seq> BC 02 04 64 00 01 00 <crc>
```

The 4-byte slot is `<opcode> 00 <value> 00`. **Idle replies fill it with zeros.**

**Every commanded change in the set was preceded by exactly this pattern, 0.176 s before the D0
announcement, and the value matched — 6/6 commands:**

| D1 `BC` reply slot (on D1) | → | D0 announcement 0.176 s later |
|---|---|---|
| `02 04 67 00 03 00` | → | `0xE1` `00 01 03` |
| `02 04 67 00 02 00` | → | `0xE1` `00 01 02` |
| `02 04 67 00 01 00` | → | `0xE1` `00 01 01` |
| `02 04 67 00 00 00` | → | `0xE1` `00 01 00` |
| `02 04 67 00 04 00` | → | `0xE1` `00 01 04` |
| `02 04 64 00 01 00` | → | `0xE4` `00 01 01` |
| `02 04 64 00 00 00` | → | `0xE4` `00 01 00` |

**6/6 commands. Gap 0.175–0.176 s every time.** The **module-side opcode is `0x67` for height and `0x64`
for flame** — verified **4/4** and **1/1** respectively against the raw frames.

> ### ⚠️ `0x67`/`0x64` and `0xE1`/`0xE4` are TWO UNRELATED NAMESPACES
>
> **Module event opcodes (`0x67`, `0x64`) vs ECM announcement bytes (`0xE1`, `0xE4`).** There is **no
> fixed transform between them — do not state one.** They are learned empirically.

> ### ⭐ Consequence for implementers — a design constraint (not a "blocker")
>
> **An ESP32 replacing the module cannot "send a command frame".** It must **wait for a `BC` request and
> answer it with the event payload in the data slot.**
>
> *Heading relabelled 2026-10-16: this is a real design constraint, but the word "blocker" now belongs to
> the refuted timing claim (see the ⭐ timing section) — the two should not read alike. The constraint
> itself is unchanged and correct.*

### ⚠️ Correlated but UNINTERPRETED — the constant `00` in the slot

The slot shape `<opcode> 00 <value> 00` contains a **constant `00` between the opcode and the value**.
Its **meaning is unknown.** It is a *correlation*, not an interpretation. **Do not guess it.**

---

## ⭐ REPLY ECHO RULE — verified 642/642 pairs

**Every D1 reply echoes the FIRST TWO PAYLOAD BYTES of its request:**

| request | reply |
|---|---|
| `BC` `02 04` | `02 04 00 00 00 00` |
| `DC` `00 17 …` | `00 17` |
| `64` `00 01 64` | `00 01` |
| `6E` `00 01` | `00 01 00` |

Verified on **642 of 642 exchange pairs** in a live capture. So the `02 04` at the head of a `BC` reply
is a **mechanical echo of the request, NOT a semantic header.**

> ⚠️ **If any text calls `02 04` a header or an opcode, that is wrong.** It is the request's first two
> payload bytes, repeated back. The module's actual write data begins **after** it.

**Likely explanation (HYPOTHESIS — not a finding):** the request's second byte is a **byte-count**,
i.e. "area `02`, up to `04` bytes" — which fits the fixed 4-byte data slot exactly. Labelled a
hypothesis because it is a reasonable fit, not a proven field.

---

## ⭐⭐ `0x20` — the ECM's POWER-ON (boot) frame — CONFIRMED

**New, 2026-10-16. This is a boot frame, not a routine command and not a state announcement.**

Capture `captures/run-manual-20261016-112657/` (1 MHz, both channels, **listen-only**) was taken across
a **manual ECM power cycle**. The first frame after power-up:

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
| `cmd` | `0x20` = **power-on / boot frame** | **confirmed** |
| `dev` | `0x01` on D0, `0x03` on D1 (the usual +2 offset) | confirmed |
| `seq` | `0x00` — the first value after a reset | confirmed |
| D0 payload | `03 01 01` | observed — **meaning UNKNOWN, do not interpret it** |
| D1 reply payload | `03 01` | **echo** of the request's first two payload bytes (echo rule, above) |

**Why CONFIRMED** (2 observations, but decisive):

1. **Both occurrences are byte-identical** — same `cmd`, `seq`, payload and CRC.
2. **Both are the FIRST frame after a boot**, both at `seq=0x00`.
3. ⭐ **`0x20` appears in NO other capture** — checked across the three earlier captures (3,310 frames
   between them, whose censuses were `{BC, DC, 64, 6E}` plus a handful of `E1`/`E4`) and nowhere else.
   A command byte that appears **only at power-up and nowhere else** is a strong signal.

**The echo relationship is an observation, not an interpretation.** D1's reply payload `03 01` echoes
the request's first two payload bytes, consistent with the verified echo rule (**642/642**). So the
request content is `03 01` (echoed) plus one further byte, `01`.

> ⚠️ **Do NOT interpret what `03 01 01` means.** The echo relationship is observed; the *semantics* of
> the three payload bytes are not known.

> ⚠️ **A note on *who* is booting.** `0x20` transmits on **D0 = ECM→module**, so the frame directly
> evidences that **the ECM emitted its power-on frame**. Whether the ECM resets *together with* the
> module (the ECM supplies the module — `pin 3 = +5 V`) or emits `0x20` *in response to* a module boot
> is **not established by this capture. Do not state which.**

### ⭐ `seq` restarts at `0x00` at power-up

Immediately after the boot frame the normal rotation resumes, and the counter increments from zero:

```
t=57.070  seq=0x00  0x20        (boot frame)
t=57.239  seq=0x01  BC
t=57.811  seq=0x02  BC
t=58.160  seq=0x03  DC
t=58.576  seq=0x04  BC
```

**The counter resets to `0x00` at power-up** — a **distinct event** from the timeout-driven park below
(a boot reset starts a fresh counter; a park *holds* `0` while the ECM keeps polling),
and **further evidence the exchange counter is owned by the ECM** (the ECM initiates and sets the
sequence; the module answers with the same value).

### ⭐ The module booted TWICE inside one capture — and one boot was spontaneous

This is the failure the whole project exists to address, **observed in the act**. Both `0x20` occurrences
are in the *same* 427.8 s capture:

| # | Boot at | Trigger | Evidence in the capture |
|---|---|---|---|
| 1 | **t=12.589** | **spontaneous — no operator action** | The capture's **first frame**. D0 carried no traffic for the first 12.589 s of the capture, and `/poll` was unreachable at the harness's pre-flight check. |
| 2 | **t=57.070** | **operator's manual power cycle** | An **11.25 s silence on BOTH lines** — D0 t=45.821→57.070, D1 t=45.854→57.080. |

**Independently corroborated, two ways:**

- **Home Assistant's `sensor.intellifire_uptime`** for the module read `2026-10-16T11:15:34Z` — a boot
  timestamp — during this period. ⚠️ That timestamp is ~11 min *before* this run started
  (`2026-10-16T11:26:57Z`, per the run's own `run.json`), so it corroborates **recurrent** rebooting
  rather than pinning either of the two in-capture events.
- ⭐ **The module's own `/poll`, from this very run.** Its `uptime`/`downtime` pair is a boot witness:

  | run t | `uptime` | `downtime` | reading |
  |---|---|---|---|
  | 20.44 → 41.64 | **0** | 14 → 36 (rising) | module **not** started; downtime counter running |
  | *(gap 25.45 s — `/poll` unreachable)* | | | spans the power cycle |
  | 67.09, 73.17 | **0** | 17 → 23 | restarted, still not up |
  | 78.47 | **28** | **0** | module finally up; counter begins |

  **`uptime` stayed at `0` with `downtime` rising across the whole boot window** — direct module-side
  evidence, no HA involvement.

### ⚠️ The module was REBOOTING, not hung

Concurrently the module's local HTTP API (`/poll`) was **dead** — no response, and **all of TCP 80/443/8080
closed** — while it **still answered ICMP ping** and its cloud link **reported connected**. A hung module
would not answer ping or report a live cloud link; a module in a reboot loop does both between restarts.

> ⚠️ **The most probable reading is that the module was rebooting repeatedly, not hung.** Marked as the
> most probable reading, **not a confirmed mechanism.**

---

## ⭐ TIMING — three numbers, and only ONE of them is a deadline

> ### ⚠️ Corrected 2026-10-16 — this section called itself a "BUILD BLOCKER" and is not
>
> It was headed *"TIMING CONSTRAINT — the reply must begin almost immediately (BUILD BLOCKER)"* and said
> *"ESPHome's default loop will not meet this"*, forcing bare-metal. **That framing was wrong twice
> over:** it treated *observed stock behaviour* as an *enforced deadline*, and it never carried the later
> finding that the ECM will wait **~5 s**. **The three numbers are separated below.** The measurement and
> the start-to-start vs end-to-start distinction are kept — both were correct.

| number | what it actually is | status |
|---|---|---|
| **41–114 µs** | how *fast the stock module* answers. **Observed behaviour of that hardware — NOT a requirement.** | measured |
| **~160 ms** | ⭐ **the real deadline.** End of one ECM frame → start of the next. The reply must be **on the wire inside that window** — *before the ECM sends its next request*. | **measured** |
| **~5 s** | how long the ECM will *wait* before parking its counter and accepting a backlog flush. **A tolerance, not a target.** | measured |

### ⭐ The real invariant — identical across three independent captures

> **End of one ECM frame → start of the next ECM frame:**
> **min 158.6–159.6 ms · p1 0.160 s · median 0.340 s**
> — identical across three independent captures (622 / 1237 / 1084 samples).

**The module's reply must be on the wire inside that ~160 ms window** — before the ECM sends its next
request. Reproducible from the per-frame timestamps in the capture frame lists
(`captures/*/frames_D0.jsonl`).

### The two earlier measurements (both kept — they are correct)

| Measurement | Basis | Value |
|---|---|---|
| **end-of-request → start-of-reply** | direct, per frame type | **41 µs (`BC`)**, **50 µs (`6E`)**, **114 µs (`DC`)** |
| *start-to-start* (the frequently-quoted figure) | D0 start vs D1 start | 8.374 ms / 8.383 ms / 32.406 ms — i.e. **≈ one D0 frame duration, ratio ~1.005** |

⚠️ **These are two different measurements of the same back-to-back exchange — do not conflate them.**
The *start-to-start* figure includes the whole request frame (at 9600-8-N-1 a byte is 10 bit-periods:
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

## ECM state announcements — two decoded, four routine frames unknown, plus the `0x20` boot frame

The bus carries two classes of traffic: **ECM state announcements** (rare — only when the fireplace's
state actually changes, driven from the app or Home Assistant) and **routine frames** (constant, and the
great majority of traffic).

> **Renamed.** This section used to be headed "Commands — two decoded, four unknown", and the two frames
> below were presented as commands. They are **D0 frames: the ECM announcing its state to the module.**
> The *module-side* writes that precede them live in the write-path section above.

### ⭐ `0xE4` = flame ANNOUNCEMENT — CONFIRMED

```
FLAME ON   t=95.671   D0  4B 01 9D E4 00 01 01 FA A9     payload 00 01 01
FLAME ON   t=109.540  D0  4B 01 BE E4 00 01 01 3F 6E     payload 00 01 01
FLAME OFF  t=35.393   D0  4B 01 EB E4 00 01 00 F2 A2     payload 00 01 00
```

**Payload byte 2 is the value: `0x01` = flame on, `0x00` = flame off.** Three observations of the same
command byte, covering **both** transitions — that is why this is *confirmed* rather than merely
correlated.

The D1 reply at the same `seq` is **shortened** and carries no value byte:

```
t=95.680  D1  4B 03 9D E4 00 01 E5 FB        <- reply to the flame-on above
```

Its payload `00 01` is exactly the **echo** of the request payload's first two bytes (echo rule above).

### ⭐ `0xE1` = flame height ANNOUNCEMENT — CONFIRMED

```
t=91.81   D0  4B 01 D3 E1 00 01 04 52 68     payload 00 01 04
```

Payload byte 2 = the height. `0x04` was announced and the status byte changed to `0x04` — and back to
`0x00` when the lowest height was commanded. **Same `00 01 <value>` payload shape as `0xE4`.**

⚠️ **Height is 0-based internally, 1-based in the app.** `/poll` reports `height` **0–4**; the Home
Assistant / app control is **1–5**. Setting app-height `5` makes `/poll` read `4` and status byte 7 read
`0x04`; app-height `1` makes `/poll` read `0` and byte 7 read `0x00`. This was previously mis-recorded as
a **"cloud staleness divergence"** between HA and `/poll` — **it is an off-by-one between two
representations, not staleness.** The module's write slot carries the **internal** value.

⚠️ **An earlier worklog entry recorded `0xE4` as "still unproven"** — *whether it means "flame on" or "a
state transition"*, the OFF case having failed to land inside the capture window twice. **The FLAME OFF
capture above resolves that:** the same command byte carries an explicit `0x00`/`0x01` value, so it is a
**set**, not a bare transition event.

### ⭐⭐ `0xDF` = pilot ANNOUNCEMENT — the third announcement (2026-10-16)

```
t=130.881  D0  4B 01 86 DF 00 01 01 93 8F     payload 00 01 01   pilot ON
```

Same `00 01 <value>` shape as `0xE4`/`0xE1`, and the D1 reply is shortened to `00 01` (`4B 03 86 DF 00 01
92 D2`) — the reply carries **no** value byte. This byte was **absent from this document until now**;
it is the third member of the announcement family and the one that gates ignition.

The matching **module-side write opcode is `0x65`** (pilot), in the same namespace as `0x64` (flame)
and `0x67` (height) — and, as with those, **it has no derivable relationship to the announcement
byte `0xDF`**. Do not compute one from the other.

⭐ **This capture also pinned the whole causal chain**, which had been inferred but never shown. The
app writes into the module's `0xBC` reply slot, and the ECM announces it **one exchange later** — write
at `seq N`, announcement at `seq N+1`:

| app's write (D1 `0xBC` slot) | ECM announcement (D0) | Δ | meaning |
|---|---|---|---|
| seq `0x85` `65 00 01 00` | seq `0x86` `DF 00 01 01` | **0.176 s** | pilot **on** |
| seq `0x25` `64 00 01 00` | seq `0x26` `E4 00 01 01` | **0.176 s** | flame **on** |
| seq `0x2B` `67 00 00 00` | seq `0x2C` `E1 00 01 00` | **0.176 s** | height **0** |

**Necessity and sufficiency both hold inside the capture:** of **391** D1 `0xBC` replies exactly **3**
carry a non-zero slot, and there are exactly **3** D0 announcements — 1:1, values matching. The **388**
idle slots produced **zero** announcements. The Δ is **pinned at 0.176 s** (spread ~120 µs) against
inter-frame spacing that varies 160–610 ms, so it is a fixed processing latency, not the announcement
happening to land on the next poll. Now **9/9 across sessions**.

⚠️ **Load-bearing premise:** this assumes there is **no out-of-band app→ECM path** (a cloud→RF bridge
into an ECM receiver). The capture cannot rule that out. A wall-switch ignition would be the test: it
should produce an announcement with **no** slot write.

### ⚠️⚠️ `byte7 == 4` does NOT mean "flame off" — a counterexample, captured

An earlier reading (in this project's own notes) held that height `4` is a flame-off sentinel which
lighting drops to `0`. **That is wrong.** Measured directly on this capture:

```
t=150.715  pilot=01  height=04  power=00
t=151.903  pilot=01  height=04  power=01   <-- BURNING, height still 4
t=153.021  pilot=01  height=04  power=01   <-- BURNING, height still 4
t=154.265  pilot=01  height=00  power=01   <-- only now, after the 0xE1 write
```

`4` is the **stored default restored at flame-off**, and it **persists through ignition** until a height
write lands. The `4 -> 0` transition was caused by the **app's height write 1.4 s later**, not by
flame-on. **Any implementer treating `byte7 == 4` as "flame off" will misreport state for the window
between flame-on and the first height write.** Use status byte **10** (flame) instead.

### The four routine frames — all D0 payloads CONSTANT while idle

| key | payload | total len |
|---|---|---|
| `D0/0x02/0xBC` | `02 04` | 8 |
| `D0/0x01/0xDC` | `00 17 00 00 00 00 00 04` + 17 zeros | 31 |
| `D0/0x01/0x64` | `00 01 64` | 9 |
| `D0/0x02/0x6E` | `00 01` | 8 |

D1 mirrors each with dev +2 and its own lengths: `BC` 12, `DC` 8, `64` 8, `6E` 9.

⚠️ **The *purpose* of none of these four command bytes is known.** They dominate the bus (`BC` is roughly
50% of frames). **Do not guess** — these need their own sweep. (Also note: the `DC` frame's *payload* is
mapped below, but the `DC` *command's* role is not.)

**One exception, and it matters:** the **`BC` reply is the module's write channel** (above) — so `BC` is
*confirmed* as the carrier of every module-side write, while **what the `BC` command byte means to the
ECM remains unknown.**

⚠️ These are the **D0** payloads. The D1 `BC` **reply** is *not* constant — that is where the module's
write slot varies.

**Inter-frame gaps:** 0.169 s – 0.613 s, most common ~0.18 s and ~0.61 s.
Capture span 0.549 s → 17.385 s.

### ⭐ Separately: `0x20` — the boot frame (NOT a routine frame)

Listed here so the census is complete. `0x20` is **not** a routine command and **not** an announcement:
it fires **once per boot** and appears in no other capture. ⚠️ **The four routine frames above remain
the only unidentified *routine* frames.** `0x20` moved out of that column, not into it: its **role**
(boot) is identified, its **payload** is not — see the `0x20` section above. **Note the distinction
explicitly when reading any census.**

| key | payload | total len | D1 reply len | status |
|---|---|---|---|---|
| `D0/0x01/0x20` | `03 01 01` | 9 | 8 | ⭐ **identified — the ECM's POWER-ON / boot frame** (2 observations, byte-identical; payload **not** interpreted) |

### ⭐ Status frame `D0/0x01/0xDC` — the state report

Payload indices, 0-based. The idle payload is `00 17 00 00 00 00 00 04` + 17 zeros.

| index | value | correlates with | confidence |
|---|---|---|---|
| 1 | `0x17` = 23 = `len(payload) − 2` | a length/count field. Holds for the dev-`0x01` frames but **NOT** for dev-`0x02` | likely |
| 5 | `0x01` pilot on / `0x00` pilot off | `/poll` `pilot` | **confirmed** (3 runs + idle; was "strong") |
| 7 | `0x00`–`0x04` | `/poll` `height` (internal 0-based) | **confirmed** |
| 9 | `0x00`/`0x01` | `/poll` `hot` | probable (3 runs) |
| 10 | `0x00`/`0x01` | `/poll` `power` (flame on/off) | **confirmed** (3 runs, both directions; was "strong") |

**This single frame is the state report.** Four of its fields are pinned to `/poll`; the rest of the
27-byte payload is unmapped.

⚠️ **This frame is itself evidence for the direction:** it reports ECM-internal sensor knowledge
(pilot, height, and the `hot` thermal state) that the module has no channel to learn. It is on D0 — so
D0 is the ECM.

---

## ✅ D1 is the REPLY direction — and D0 = ECM, D1 = module (corrected, then confirmed)

> **Corrected twice.** This section was first titled "*⚠️ D1 is an INDEPENDENT line — not a copy of D0*"
> and said D1 was a genuinely separate signal carrying a "lock-step parallel stream", listing candidate
> hypotheses (module TX / second ECM UART / second accessory bus / echo-loopback) with none confirmed.
> That framing was wrong: D1 is not an independent parallel stream, it is **the reply direction** of a
> single request/response link. **It is now further resolved: the polarity is confirmed** — D0 is the
> **ECM→module** direction and D1 is the **module→ECM** direction (evidence in the read-first block at
> the top).

### The evidence: D1 answers D0 back-to-back

Measured from `dec3_D0.txt` / `dec3_D1.txt` (D0 start time vs D1 start time vs one D0 frame duration; at
9600 8N1 a byte is 10 bit-periods):

| exchange | D0 starts | D1 starts | delta | D0 frame duration | ratio |
|---|---|---|---|---|---|
| BC (8 B) | 0.019821 | 0.028195 | 8.374 ms | 8.333 ms | **1.005** |
| DC (31 B) | 0.411869 | 0.444275 | 32.406 ms | 32.292 ms | **1.004** |
| 6E (8 B) | 0.967900 | 0.976283 | 8.383 ms | 8.333 ms | **1.006** |

**D1's frame begins EXACTLY one D0-frame-duration after D0's frame begins, in every case** (ratio
~1.005). D0 transmits and D1 answers back-to-back. This is a **request/response link**, not two
concurrent streams.

⚠️ **The delta column above is a *start-to-start* measurement.** Subtracting the frame duration gives the
**end-of-request → start-of-reply** gap: **41 µs (`BC`), 50 µs (`6E`), 114 µs (`DC`)**. Same exchange,
two ways of measuring it — do not conflate them (see the timing section). ⭐ **Note the relabel:** the
end-to-start figure is *observed stock behaviour*, **not** the constraint. The **real constraint is the
~160 ms inter-frame window** (end of one ECM frame → start of the next) — corrected 2026-10-16.

### This also explains the "no overlapping edges" observation

An earlier reading noted that **0 of D1's edges coincide with D0's**. That is exactly what back-to-back
request/response looks like — **the two lines do not overlap in time.** It is consistent with
request/response and is **not** evidence of independence. (The other numbers quoted alongside it —
transition counts, time-shift correlation — are dump1-derived and unreliable; see the ⚠️ warning at the
top.)

### The replies pair with their requests (dump3)

| seq | D0 (request) | D1 (reply) |
|---|---|---|
| `98` | dev=`02` cmd=`BC` payload `02 04` (8 B) | dev=`04` cmd=`BC` payload `02 04 00 00 00 00` (12 B) |
| `99` | dev=`01` cmd=`DC` payload 25 B (31 B total) | dev=`03` cmd=`DC` payload `00 17` (8 B) |
| `9A` | dev=`02` cmd=`BC` payload `02 04` (8 B) | dev=`04` cmd=`BC` payload `02 04 00 00 00 00` (12 B) |
| `9B` | dev=`02` cmd=`6E` payload `00 01` (8 B) | dev=`04` cmd=`6E` payload `00 01 00 99` (12 B) |

- **Device IDs offset by +2** (01→03, 02→04) — the two ends address each other under offset ids
- **Same `seq`, same command, same order** — because each reply answers its request
- **Different lengths and payloads** — the reply is not a copy of the request
- ⭐ **Every reply's first two payload bytes are the request's first two payload bytes** — the echo rule,
  which holds on **642/642** pairs. So `02 04` in a `BC` reply is **echo, not header**
- **Both CRC-valid**

**Why the reply to the 31-byte `DC` frame is only 8 bytes (payload `00 17`):** it is a short
acknowledgement, not a mirrored payload. A 31-byte request answered by an 8-byte ack is normal for
request/response and inexplicable for a "parallel stream".

### ✅ Direction — now resolved by content

> **Superseded:** this subsection used to be headed "*What is STILL unproven*" and read: "*The direction
> assignment is inferred from timing and has NOT been confirmed by content. Timing shows* that *one line
> answers the other; it does not, on its own, say which physical endpoint is speaking on which line.*"
> and "*Do not treat the D0/D1 → pin 6/pin 4 mapping as settled.*"

**Both questions are now settled.** The module-absent capture, the ECM-internal content of the `DC`
status frame, and the 0.176 s causality all point the same way:

| Line | Captured on | Direction | Status |
|---|---|---|---|
| **D0** | ECM **pin 6** | **ECM → module** | **confirmed** |
| **D1** | ECM **pin 4** | **module → ECM** | **confirmed** |

This also **upgrades the pin-6 reading in `docs/ECM_PORT_PINOUT.md`** from "candidate TX" to a confirmed
ECM serial **TX** — the mirror mapping derived from the community capture was right, and the analyzer
capture
settled the "traffic or meter noise?" question it left open.

---

## ✅ UPDATE — dump3 (1 MHz) validates completely

A second capture (`logic_analyzer_data/dump3.vcd`, taken at **1 MHz**) decodes cleanly and **every
frame CRC-validates** — using sigrok-cli's standard decoder, no phase tuning.

```
D0:  ✅ len=8   4B 02 98 BC 02 04 98 47
     ✅ len=31  4B 01 99 DC 00 17 00 00 00 00 00 04 00×17 F5 34
     ✅ len=8   4B 02 9A BC 02 04 99 FF

D1:  ✅ len=12  4B 04 98 BC 02 04 00 00 00 00 B4 D5
     ✅ len=8   4B 03 99 DC 00 17 E4 C8
     ✅ len=12  4B 04 9A BC 02 04 00 00 00 00 35 0C
```

### This settles the sample-rate question definitively

| Dump | Rate | sigrok-cli default decode | Verdict |
|---|---|---|---|
| **dump1** | 20 kHz (2.1 samples/bit) | **no CRC matches** | ⚠️ **unreliable — decode corrupt; do not use for ANY structural claim** |
| **dump3** | **1 MHz (104 samples/bit)** | **all frames validate** | ✅ **authoritative — use this rate** |

**At 1 MHz the phase sensitivity disappears entirely.** Recapture at ≥250 kHz, ideally 1 MHz.

**⚠️ dump1's D1 stream is specifically corrupt.** `sig_dump1_D1.txt` decodes device bytes as `0x84`,
`0x83`, `0xF0`, but device ids on D1 must be `0x03`/`0x04` — those are **bit errors**, not a protocol
feature. dump3 decodes every frame cleanly with the same decoder and no phase tuning. **Any dump1-derived
claim in this document has been marked as corrected or superseded.**

### ⭐ D1 is the reply direction — reproduced at 1 MHz

> **Corrected:** this heading previously read "*The D1 parallel stream is CONFIRMED, not an artefact*".
> The pairing is indeed real and reproducible — but it is **request/response**, not a parallel stream.

Reproduced independently at 1 MHz with the standard decoder (the pairing table is in the D1 section
above — the two directions carry the **same `seq`**, which is the exchange counter / join key).
**Same `seq`, same commands, same order — device IDs offset by +2, payload lengths differ.** Both
CRC-valid. This is real and reproducible, and it is the signature of a request/response link.

### Tooling corrections

- **`sigrok-cli` DOES work on this machine** — at `~/.local/bin/sigrok-cli` (a wrapper for
  `/Applications/sigrok-cli.app`). Version `0.8.0-git`. An earlier conclusion that Homebrew had failed
  was wrong.
- ⚠️ **The wrapper's launcher `cd`s into its own bundle directory, so RELATIVE PATHS BREAK.** Always
  pass absolute paths, or you get `Not a regular file`.
- Correct flag is **`-P` / `--protocol-decoders`**, not `--protocol-decoder`.
- Decode command:
  ```sh
  sigrok-cli -i /abs/path/dump.sr -P uart:baudrate=9600:rx=D0 -A uart=rx-data
  ```
- **Fixed a bug in `tools/ecm_analyse.py`:** its frame-length search stopped at 24 bytes, so it silently
  skipped the **31-byte `DC` status frame**. Limit raised to 80.

#### Confirmed tooling facts

Two facts were verified and are now pinned in the tools:

| Fact | Detail |
|---|---|
| **Absolute paths are mandatory** | The `sigrok-cli` wrapper `cd`s into its own app bundle, so a **relative path fails** with `Not a regular file`. Always pass an absolute path. |
| **The flag is `-P` / `--protocol-decoders`** (plural) | The singular `--protocol-decoder` is **NOT a valid option** and fails with `Unknown option`. |

The flag fact was already stated correctly above and is repeated here only to record that it was a **real
bug** in `tools/ecm_analyse.py` and `tools/ecm_capture.sh`, **now fixed** (both carry the correct plural
form, and both document the absolute-path requirement at their call sites).

---

## ⭐ The `seq` reset is caused by a TIMED-OUT REPLY — not by the command

> ### ⚠️ Corrected 2026-10-16 — the cause is now known, and it is not what this section used to say
>
> This section was headed *"OPEN OBSERVATION — every state-changing command stalls the exchange
> counter"*, said the counter *"resets to `00`"* after a command, and left the **cause UNKNOWN**.
> **That was an indirect correlation mistaken for the cause.** A capture shows the mechanism frame-by-frame.

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

**The observed shape (data — unchanged):** after each state-changing command the exchange counter
**sits at `00` for ~5 s** (11–14 consecutive CRC-valid frames), then resumes. ⚠️ **Read this as the
observable, not the cause** — the cause is the reply timeout above.

| command | cmd at | stall at | delta |
|---|---|---|---|
| flame ON `0xE4` | 95.671 | 97.34 | 1.67 s |
| flame ON `0xE4` | 109.540 | 111.00 | 1.46 s |
| flame OFF `0xE4` | 35.393 | 36.894 | 1.50 s |
| height `0xE1` | 91.813 | 92.848 | 1.04 s |

**Last capture — measured after EVERY command (5), Δ 1.3–1.7 s:** `0xE1` @ `00 01 03` +1.63 s ·
`00 01 02` +1.33 s · `00 01 01` +1.72 s · `00 01 00` +1.61 s · `0xE4` @ `00 01 00` +1.46 s.

A `/poll` gap (15.8–20.1 s) is logged around the same moment. **The stall is independent of `/poll`
polling load** — it was reproduced in runs whose poll pressure differed by ~100×, and it was the
`/poll` death, not the stall, that tracked load.

**It fires for the height command as well as the flame command.** A fault specific to ignition would not
fire identically for a flame-height write. ⚠️ **The earlier guess of *"a deliberate counter resync"* is
withdrawn** — a resync would not park the counter at `0` while the ECM keeps polling; a **reply timeout**
does exactly that, and that is what the frame-by-frame sequence above shows.

⚠️ **Which side?** Now answerable: the **silence** is in the **module's transmit path** (the ECM polls
throughout), and the **counter parking** is **ECM-side response to the timeout**. So a replacement board
would **neither inherit nor need to emulate** the stall. (This paragraph previously read *"not yet known
whether this is module-side or ECM-side"* — superseded by the sequence above.)

⚠️ **Do NOT present this as a defect.** The **null test** was
proposed to name a cause; the cause is now known from the sequence above (a timed-out reply), so the null
test is no longer the blocker. What remains solid and reproducible: it follows command execution, fires
across two different command bytes, and is independent of `/poll` load.

#### ⭐ NEW — a stall with NO command at all — and it is the SAME mechanism

**One occurrence, 1/1. ⭐ Cause now known: a MISSED REPLY.** This is **the same event** that established
the timeout mechanism above — it is what a reply timeout looks like when no command precedes it.

In the boot capture the module's transmit line went silent for **4.814 s**:

| Element | Value |
|---|---|
| Last D1 frame before silence | `BC` reply at **t=70.075** |
| D1 resumes | **t=74.889**, with `DC` at **seq=36** — answering the outstanding D0 exchange |
| Nearest preceding D0 frames | `0x6E` t=69.583 · `0x64` t=69.746 · `BC` t=70.051 · `DC` t=70.400 — **all routine** |
| `0xE1` / `0xE4` in this capture | **zero** — no flame or height traffic appears at all |

It is the **same shape** as the stall above (the counter parks at `00`): while D1 was silent, **D0 kept
polling normally at its usual ~0.3–0.6 s spacing with `seq` parked at `0x00`** (t=70.817 → t=74.737),
then D1 answered the outstanding `seq=36` and resynced to `0x00`.

⭐ **Both earlier candidate readings resolve cleanly against the mechanism above:**

1. **"A trigger besides state-changing commands"** — ✅ **met: a missed reply.** The `seq=0x24` `DC`
   request at t=70.400 went unanswered, the ECM timed out, parked the counter at `0`, and the module
   answered 4.5 s late.
2. **"The module was still settling ~13 s after its boot"** — still open as the **reason the reply was
   missed**, but it is **not** the mechanism of the reset. The reset mechanism is the timeout.

⚠️ **Not** a defect.

---

## ✅ RESOLVED — the D1 reply to `BC` is the module's WRITE CHANNEL

> **Updated.** This section was headed "⚠️ Correlated but UNINTERPRETED — the `64 00 <bit>` extra bytes"
> and said nobody knew what the extra bytes meant. **They are now interpreted:** they are the module's
> 4-byte write slot, and the changed byte is the value being written.

At a flame transition, the D1 reply to the *preceding* `BC` frame carries extra bytes:

| moment | D1 reply payload to `BC` | read as |
|---|---|---|
| idle | `02 04 00 00 00 00` | echo `02 04` + **zeroed slot** |
| flame **on** | `02 04 64 00 01 00` | echo `02 04` + slot `64 00 01 00` → opcode `0x64`, value `0x01` |
| flame **off** | `02 04 64 00 00 00` | echo `02 04` + slot `64 00 00 00` → opcode `0x64`, value `0x00` |

**The old open question — "whether `0x64` here is the same thing as the `0x64` *command byte*, and
whether the module is echoing state or re-encoding it" — is answered:** it is the **module's write
opcode**, and it is **not** the `0x64` routine command byte (two unrelated namespaces — see the warning
in the write-path section).

---

## How these were obtained

| Element | Detail |
|---|---|
| **Harness** | `tools/ecm_harness.py` — **listen-only**. It never transmits; the bus is driven only by the fireplace's own control path (Home Assistant / the app) |
| **Capture** | 1 MHz logic-analyser capture on **ECM pins 4 and 6**, **GND on pin 2/8** (continuity pin 2 ↔ pin 8 confirmed in `docs/ECM_PORT_PINOUT.md`) |
| **Read-path answer key** | `GET http://<module>/poll` — polled alongside each capture so every changed frame can be tied to a real fireplace value. See the note on the stock module's local API in [PROTOCOL_DECODED.md](PROTOCOL_DECODED.md) |
| **Flame data** | The **fireplace was actually ignited** to obtain the `0xE4` flame and `0xE1` height evidence. This is live gas-appliance data, not a simulation |
| **Trigger correlation** | The harness fires the mark at a known **offset into** the capture and finds the change by **content** (first frame differing from baseline), never by timestamp — `.sr` files carry no reliable wall-clock start |
| **Write-path correlation** | The slot value in the D1 `BC` reply is matched to the D0 announcement **0.176 s later, by value** — 6/6 commands, gap 0.175–0.176 s |
| **Echo rule** | Counted across **642/642 exchange pairs** in a live capture |
| **Boot capture** | `captures/run-manual-20261016-112657/` — 1 MHz, both channels, **listen-only**, taken **across a manual ECM power cycle**. The operator drove the **power cycle by hand while a long capture rolled** — the **reliable method** (paced background timers repeatedly failed: notifications batch and arrive late, and a commanded change repeatedly landed *after* the capture window closed). ⚠️ The harness flagged this run `CAPTURE_SHORT` (427.8 s of a planned 900 s), but **every frame present is CRC-valid** and both boot events fall inside the window. |
| **Read path vs. serial link** | ⚠️ **They fail independently.** This run required a new `--allow-no-poll` flag because **`/poll` was down while the serial link was perfectly healthy** — the harness aborts without it, since it treats the read path as the answer key. |

> ### ⚠️ SAFETY — this is listen-only analysis
>
> **`/poll` is the read path and is safe. `/post` is the WRITE path and must NEVER be called in a
> read-only context** — every field it can change drives the actual fireplace.
>
> Analyzer ground connects **first** and is removed **last**. **Never clip the analyzer to ECM pin 3
> (+5 V supply).** Keep the RF remote's batteries out for the session. Mains is live in the cavity —
> ECM low-voltage side only.

---

## ⚠️ Open questions

- **How long the ECM waits before it parks its counter, and whether a *late slot write* is still
  honoured.** The shape is now known (it keeps polling with `seq` parked at `0`, then resyncs from `1`),
  but the timeout value, and whether the ECM still *acts on* a late write, are not.
- **Whether the ECM *enforces* the ~160 ms window**, or merely happens to receive replies inside it.
  ⚠️ **Partly answered:** the ECM demonstrably tolerates **~5 s** of silence and accepts a backlog flush,
  so **~160 ms is the design budget, not a hard enforced deadline** — a replacement has headroom.
- **Whether the announcements also appear for changes initiated by the wall switch or the RF remote.**
  Nobody has captured that — and it would **cleanly confirm the ECM-origin reading**, since a wall-switch
  or RF change originates at the ECM and not in the app.
- **What the *purpose* of `BC`, the `DC` command-role, `0x64` and `0x6E` is.** They dominate the bus.
  (The `DC` *payload* is mapped, and the `BC` *reply* is known to be the module's write channel; the
  command bytes themselves are not.)
- **The constant `00` inside the write slot** (`<opcode> 00 <value> 00`) — correlated, uninterpreted.
- **Whether the module's transmit-path stall** (the ~5 s silence itself, *not* the counter reset) is
  benign — the reset's cause is known, but why a write makes the module's transmit path go quiet is not.
- **What `0x20`'s payload `03 01 01` means.** The frame's **role** (boot) is identified; the **payload** is
  not. **Do not guess it.**
- **Whether the ECM resets *with* the module, or emits `0x20` *in response to* a module boot.** The frame
  is on D0, so the capture shows only that the ECM emitted it. Unknown.
- **Whether `0x20` is emitted at *every* boot** — two observations, both at a boot, no counterexample, but
  only two.
- **Why the reply at t=70.400 was missed** (the 4.814 s stall with no preceding command) — ⭐ the *reset
  mechanism* is known (a timed-out reply), but the reason that one reply never came is not.
- **Whether height is only settable while burning.** Setting the height with the flame **off** produced
  **no bus traffic at all** — no frame reached the module. Whether that is a firmware guard or a harness
  artefact is unknown.

**Resolved and removed from this list:** which D0/D1 line is which direction (**D0 = ECM→module,
D1 = module→ECM**); the reply encoding (**the echo rule, 642/642**); the `64 00 <bit>` extra bytes
(**the module's write slot**); the D0/D1 → pin 6/pin 4 mapping (**confirmed**); the role of `0x20`
(**the ECM's power-on / boot frame**); whether `seq` restarts at boot (**yes — `0x00`**).

---

## What this means

**The framing layer is solved, the direction is resolved, and the module's write path is decoded.** On
the authority of **dump3 (1 MHz)**, not dump1: start byte, field layout, checksum, the two-line
request/response structure with a shared wrapping exchange counter, four fireplace-state status fields
(pilot, height, hot, power), the **`0xE4` flame** and **`0xE1` height** announcements, the **echo rule**,
and — the part that changes the build — **the module's only write channel being the 4-byte slot in its
reply to `BC`**. **New (2026-10-16):** the ECM's **`0x20` power-on / boot frame** is identified, and a
capture across a manual power cycle **caught the module rebooting twice — once spontaneously**. ⭐ **Also
corrected 2026-10-16:** the **41–114 µs reply gap is *observed stock behaviour*, not a deadline** — the
real design budget is **~160 ms**, the ECM tolerates **~5 s** of silence, and **ESPHome is therefore
viable** (see the ⭐ timing section). The remaining work is the **purpose of the four routine frames** and
the ECM's behaviour when the module misbehaves.

**What is CONFIRMED:** the frame format; CRC; `seq` as a wrapping exchange counter that is the join key,
which **resets to `0` and parks while replies time out, then resumes from `1`** — and **resets to `0` at
power-up**; **D0 = ECM→module and D1 = module→ECM**;
the reply echo rule (642/642); flame and height both settable, with `0xE4`/`0xE1` **announcements**
carrying the value; the module's write path being the `BC` reply data slot (6/6 events); status byte 7 =
height (0–4 internal); byte 10 = power; byte 5 = pilot; byte 9 = hot (probable); ⭐ **`0x20` = the ECM's
power-on / boot frame** (2 observations, byte-identical, absent from every other capture).

**What is CORRELATED, UNINTERPRETED:** the meaning of the constant `00` between opcode and value in the
event slot; status byte 1 (length/count); ⭐ **`0x20`'s payload `03 01 01`** (the echo relationship is
observed, the semantics are not). **Do not guess — in particular, do not guess the `00` or the
`03 01 01`.** (The **`seq` stall** used to be listed here; its cause is now known — a **timed-out reply**
— and it is listed under CONFIRMED above.)

**What is UNKNOWN:** the *purpose* of the four routine command bytes; **how long the ECM waits before it
parks its counter, and whether it still acts on a *late* write** (the shape is now known — it keeps
polling with `seq` at `0`, then resyncs from `1`); **whether the ~160 ms window is *enforced*** (⚠️ the
ECM demonstrably tolerates ~5 s, so it is a design budget, not a hard deadline); whether wall-switch/RF
changes produce the same announcements; **the rest of the boot handshake** (its first frame, `0x20`, is
now identified — its payload and what follows are not) and **whether the ECM resets with the module**;
why a write makes the **module's transmit path** go quiet; why the reply at t=70.400 was missed;
whether height is only settable while burning.

### Next steps

1. **Capture at ≥250 kHz (ideally 1 MHz) with lsb-first for every future capture.** dump3 (1 MHz) already
   does this and validates every frame; **dump1 (20 kHz) must not be re-used for analysis.**
2. ⭐ **Map the *purpose* of the four routine frames** — `BC` (beyond its reply being the write channel),
   the `DC` command-role, `0x64`, `0x6E`. They dominate the bus; this is now the largest remaining gap.
3. ⭐ **Capture a change made from the wall switch or the RF remote.** If the same `0xE4`/`0xE1`
   announcements appear on D0 for a change the app did not originate, the ECM-origin reading is
   confirmed outright — and if the `BC` reply slot stays zeroed through it, the write path is confirmed
   as the module's channel specifically.
4. **Capture what the ECM does when the replies stop** — stop answering `BC` and watch D0. ⭐ **Partly
   answered 2026-10-16:** the ECM parks its counter at `0`, keeps polling, and resyncs from `1` (see the
   `seq` timeout section). **Still needed:** how long it waits, and whether it still acts on a late write.
5. ⭐ **Time the stall precisely / run a no-op control** — the **cause** of the `seq` reset is now known
   (a timed-out reply), so the null test is **no longer needed to name the cause**. What it can still
   settle is why a *write* makes the **module's transmit path** go quiet in the first place.
6. **Capture through a reboot for the startup handshake** — ⭐ **partly done 2026-10-16:** the ECM's
   **`0x20` boot frame** is now identified (`captures/run-manual-20261016-112657/`). **Still needed:** a
   capture that starts *before* the ECM comes up, to see what (if anything) precedes `0x20`; and a
   **third, independent boot** to test whether `0x20` is invariant and always emitted.
7. ⭐ **Boot the ECM deliberately, without disturbing the module** — this is the cheap test for the open
   question of whether the ECM resets *with* the module or emits `0x20` *in response to* a module boot.
   If a **module-only** reboot produces `0x20` on D0, the ECM is reacting to the module; if it does not,
   the two reset together.

### Reproduce this analysis

```sh
# Decode + CRC-validate. Use dump3 (1 MHz) — dump1's decode is corrupt.
# sigrok-cli needs an ABSOLUTE path, and the flag is -P (plural).
sigrok-cli -i /abs/path/logic_analyzer_data/dump3.vcd -P uart:baudrate=9600:rx=D0 -A uart=rx-data
```

**Boot capture (the `0x20` evidence, 2026-10-16):** the original 1 MHz sigrok recording is
not included in this repository, but its extracted frames are:
`captures/run-manual-20261016-112657/frames_D0.jsonl` and `frames_D1.jsonl`. Both boot
events fall inside that capture. Parse or replay them with:

```sh
python3 tools/ecm_frames.py captures/run-manual-20261016-112657/frames_D0.jsonl
make -C firmware test-replay CAPTURES=../captures
```

⚠️ **If you use `sigrok-cli` on an `.sr` file, use an absolute path** — its wrapper `cd`s
into its own app bundle, and a relative path fails with `Not a regular file` and **empty
output**, which is indistinguishable from a silent bus.

Authoritative decodes: `logic_analyzer_data/sig_dump3_D0.txt` · `logic_analyzer_data/sig_dump3_D1.txt` ·
`logic_analyzer_data/dec3_D0.txt` · `logic_analyzer_data/dec3_D1.txt`. The older
dump1-derived `frames_analysis.txt` and `timing_analysis.txt` are not included; they were
superseded by the dump3 decodes.
