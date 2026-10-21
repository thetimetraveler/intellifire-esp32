# ECM 8-pin Wi-Fi port — measured pinout

Measured 2026-10-16 with the ECM installed, selector **IFT-REM**, the 6 V adapter
connected, and a **Klein MM300** on DC volts (20 V range), black probe on the
fireplace chassis.

> **Provenance caveat.** The original note recorded the measurement as taken with
> the module **removed**. A later review found no independent record of that
> condition, and the operator believed the module may have been **seated** for
> the reading. The values below are unchanged and are not withdrawn; what is
> uncertain is the condition they were taken under. Do not lean on the claim that
> the port was powered without a module. No DC-current measurement of this port
> exists.

## Measured readings

| Pin | Voltage | Behaviour | Interpretation | Confidence |
|---|---|---|---|---|
| 1 | 0.07 V | steady | **not ground — likely unconnected/NC** (failed the 5 V test) | resolved |
| **2** | 0.00 V | steady | **ground — confirmed** | **confirmed** |
| **3** | **5.07 V** | **steady** | **+5 V supply — do not connect** | **strong** |
| 4 | 3.3 V | changing | **data line** (active) — see the contradiction note | **unconfirmed — see note** |
| 5 | 3.31 V | constant | idle-high **input** (ECM RX with pull-up?) or 3.3 V rail | plausible |
| **6** | **3.2 V** | changing | **data — confirmed: ECM serial TX** | **confirmed** |
| 7 | 0.00 V | steady | **not ground — likely unconnected/NC** (failed the 5 V test) | resolved |
| **8** | 0.02 V | steady | **ground — confirmed** | **confirmed** |

### Contradiction on pin 4

The original header says the measurement was taken with the module **removed**.
The table lists **pin 4 as "3.3 V changing — data line (active)"**. With the
module out, nothing should be driving pin 4 if it is the module's transmit line.
One of two explanations must hold, and neither is established:

1. pin 4's "changing" reading is a **floating-pin artefact** — a high-impedance
   meter wanders on a floating input, which is how pins 1 and 7 were originally
   misread as possible ground; or
2. the module was **not actually removed** for that measurement.

The evidence that pin 4 is the module's transmit line comes from the **live
captures**, where the module was present and D1 traffic was recorded on it — not
from this voltage reading. **Confirm the pin-4 ↔ module-TX assignment with a
capture before soldering to pin 4.**

### Ground confirmation

Each candidate was measured against pin 3 (the +5 V supply). A pin that completes
the 5 V circuit is the return by definition; a floating pin cannot.

| Candidate | Reading to pin 3 | Verdict |
|---|---|---|
| 1 | not 5.07 V | not ground |
| **2** | **5.07 V** | **ground** |
| 7 | not 5.07 V | not ground |
| **8** | **5.08 V** | **ground** |

This test is why the answer is trustworthy. About 0 V alone would not have
sufficed — a floating pin also reads near 0 V on a high-impedance meter, which is
exactly what pins 1 and 7 turned out to be.

### Port summary

**8 pins = 2 grounds (2, 8) + 1 supply (3) + 3 signal candidates (4, 5, 6) + 2
unconnected (1, 7).** The module needed only about 4–5 wires of the 8.

### Minor open question

Pins 2 and 8 both measured as ground, so an unpowered continuity test between
them should beep. An earlier pin-to-pin sweep on the bench gave `OL` on every
pair. Possible explanations: an earlier board state, a different board revision,
or a probe not making contact. Cheap to settle: unplug the adapter, then check
continuity between pins 2 and 8.

Readings above about 3.3 V on pins 4 and 6 are depressed relative to an idle-high
line, the expected signature of **active traffic** averaged by a DC voltmeter.

## Corroboration: the mirror mapping

A community post reported probing the same port with no module fitted and
labelled two pins:

> `5 - +5V` and `2 - Serial TX` (0-indexed, counted from the bottom)

The measurements here are 1-indexed. Two candidate mappings:

| Mapping | Their "5 → +5V" becomes | Reading | Their "2 → TX" becomes | Reading | Verdict |
|---|---|---|---|---|---|
| `n + 1` | pin 6 | 3.2 V changing — no | pin 3 | 5.07 V steady — no | fails both |
| **`8 − n`** (mirror) | **pin 3** | **5.07 V** — yes | **pin 6** | **3.2 V changing** — yes | **matches both** |

Two independently labelled pins matching under one consistent mapping is
unlikely to be coincidence. It supports:

- **Pin 3 = +5 V supply**
- **Pin 6 = ECM serial TX.** Confirmed from the decoded protocol, not from this
  measurement alone: the frames captured on D0 (pin 6) are byte-identical in
  shape to the community capture made with the module removed. Frames that exist
  with no module fitted can only have been transmitted by the ECM. The `DC`
  status frame also carries ECM-internal state that the module has no channel to
  learn.

**Corollary for a replacement board: pin 6 is the ECM's transmit line, so it is
an input to the module and must never be driven by an ESP32.** The module
transmits on **pin 4** (D1). See [COMMAND_MAP.md](COMMAND_MAP.md).

The mirror depends on which physical end each observer counted from. The two-point
match strongly suggests it; confirm by locating pin 1 on the port (silkscreen,
square pad, or keying) and checking the physical order.

## What is not yet established

- **Pin 4 = module TX is unconfirmed.** The capture-based evidence supports it,
  but a capture that confirms the physical pin is a prerequisite before
  soldering to pin 4.
- Pins 1 and 7 read near 0 V but failed the supply-return test; they are
  unconnected rather than ground. A resistance check would settle it.
- **Pin 5 could be a 3.3 V rail rather than an input.** Not distinguished yet.
- Only one measurement session, one operator, one meter.

## Confirming ground

**About 0 V alone does not prove a pin is ground.** A floating, unconnected pin
also reads near 0 V on a high-impedance meter. Use a low-impedance test that a
floating pin cannot pass.

### Method 1 — live and safe, needs no other reference

Measure each candidate against pin 3 (the confirmed +5 V supply):

- **Red probe → pin 3** (supply)
- **Black probe → each candidate** (1, 2, 7, 8) in turn

| Reading | Meaning |
|---|---|
| **~5.07 V** | that pin is the supply return = **ground** |
| ~0 V / odd | not ground — floating or a different net |

Measuring across the supply is rigorous: whatever pin completes the 5 V circuit
is the ground by definition, and a floating pin cannot complete it. No
power-down or chassis reference is needed.

### Method 2 — cross-check the zero pins

With the black probe on pin 2, measure pins 1, 7, and 8:

| Reading | Meaning |
|---|---|
| all **~0.00 V** | likely the same net — a commoned ground |
| non-zero | a different net, or floating |

Four ground pins on an 8-pin connector is plausible: the module's 52-pad package
carries 20+ GND pads, so the host port shorting several together is normal
practice.

### Method 3 — definitive, requires power-down

Unplug the 6 V adapter, then use resistance mode against a known return path:

| Measurement | Confirms ground by |
|---|---|
| candidate ↔ battery pack negative | **~0 Ω** |
| candidate ↔ DC jack sleeve | **~0 Ω** |

A ~0 Ω reading to a known return is the most rigorous confirmation available.

### Live/dead rule

| Board state | Allowed modes |
|---|---|
| Adapter connected (even selector OFF) | **DC volts only** |
| Adapter unplugged | volts **or** resistance/continuity |

The selector only kills the port; the board still has 6 V with the adapter in.
**Resistance and continuity are only valid on a dead board.**

### Which pin to clip for the analyzer

Any pin that passes Method 1. Prefer the one that read 5.07 V most cleanly and
stably. The ECM runs from an isolated 6 V supply, so it floats relative to mains
earth; the analyzer's ground clip establishes the reference for the whole
measurement.

## Next steps

### 1. Voltage dwell, not frequency

The Klein MM300 has no frequency counter. Its functions are AC/DC voltage, DC
current, resistance, battery/diode/continuity; the only "Hz" in its specification
is the 50–60 Hz AC accuracy range. Use a **voltage dwell** test instead.

The meter samples about twice per second, so give each pin real time. Set DC
volts (20 V) and watch pins 4, 5, and 6 for 30–60 seconds each:

| Behaviour | Meaning |
|---|---|
| **Wanders over a ~0.1–0.2 V band** | real traffic (the meter averages changing bit patterns) |
| **Rock steady** | a rail, or a genuinely idle line |
| **Repeats a visible pattern** | real, periodic traffic |

The existing evidence supports traffic on pins 4 and 6: they read changing while
pin 5 held a constant 3.31 V. A rail never moves, so the contrast is meaningful.

The definitive answer comes from the logic analyzer, which returns the actual
bytes rather than just "something toggles here".

### 2. Resistance between the zero pins

**Return the selector to OFF first** — that de-energises the port (the WFM port is
powered only in IFT-REM). Then measure pin 1 ↔ 2, 1 ↔ 7, and 1 ↔ 8:

| Reading | Meaning |
|---|---|
| **~0 Ω** | same net → all genuine grounds |
| open | not the same net → unconnected, not ground |

### 3. Logic analyzer on pin 6

Pin 6 is the ECM serial TX. Connect analyzer ground **first**, then CH0 → pin 6.
**Never clip to pin 3 (5 V supply)** — check the analyzer's input limit.

Decode as UART **9600-8-N-1**; look for `0x4B` frame starts and MODBUS CRC-16.

## Safety notes

- **Selector IFT-REM means the fireplace can ignite.** The RF remote and ECM
  remain able to command it. Removing the Wi-Fi module does not make the unit
  safe.
- **Pin 3 carries ~5 V and is the module's supply — never connect it** to the
  analyzer or ESP32.
- **Do not leave probes on the unit unattended** — return the selector to OFF.
- **The 120 V mains feed is live in the cavity.**
