# Logic analyzer setup — capturing the ECM port

**Analyzer:** 8-channel 24 MHz USB logic analyzer (fx2/Cypress clone).
**Software:** **PulseView** or `sigrok-cli` (sigrok). Saleae Logic 2 does not
support these clones.

## 1. Before clipping anything on

### Confirm the analyzer's input limit

These fx2 clones are commonly rated **0–5.25 V with no overvoltage protection**.
The reference measurement found **pin 3 at 5.07 V** — inside a 5.25 V limit, but
with almost no margin, and within noise of a limit that varies by board.

**Do not clip onto pin 3.** Check the stated rating of your specific analyzer
before connecting anything.

### Never clip to the supply

Pin 3 is the module's **+5 V supply**. Connecting it to the analyzer risks the
analyzer and tells you nothing about the data link.

**Only clip ground and the data-line candidates (pins 4 and 6).**

## 2. Connection order

```
   1st:  analyzer GND  ──►  ECM GROUND (pin 2 or 8 — confirmed grounds)
   2nd:  CH0           ──►  pin 6   (ECM TX)
   3rd:  CH1           ──►  pin 4   (module TX)
```

- **Connect ground first, remove it last.** A signal lead on an ungrounded
  analyzer gives phantom readings and can damage it.
- **Power the analyzer from USB on your laptop** — never from the fireplace.
- **Do not clip pin 3, pin 1, or pin 5.**

Pins 2 and 8 both measured 0.00 V and passed the supply-return test in
[ECM_PORT_PINOUT.md](ECM_PORT_PINOUT.md). Either is suitable as the analyzer
ground.

## 3. Software install

`brew install --cask pulseview` does not exist. Options:

| Route | Command | Notes |
|---|---|---|
| **sigrok-cli (Homebrew)** | `brew install sigrok-cli` | Command-line only, but it captures to a file and decodes UART. Sufficient for this job |
| **PulseView GUI** | download the macOS app from [sigrok.org/wiki/Downloads](https://sigrok.org/wiki/Downloads) | GUI, easier to inspect waveforms |
| Saleae Logic 2 | — | Does not support these clones |

If you only need the bytes, `sigrok-cli` is sufficient:

```sh
brew install sigrok-cli

# list devices
sigrok-cli --scan

# capture 30 s at 1 MHz on channels 0 and 1
sigrok-cli --driver fx2lafw --config samplerate=1m \
  --channels D0,D1 --time 30s \
  --output-file capture.sr
```

Decode UART:

```sh
sigrok-cli -i capture.sr --protocol-decoder uart:baudrate=9600:rx=D0
```

## 4. Capture settings

| Setting | Value | Why |
|---|---|---|
| **Driver** | `fx2lafw` | the clone's driver |
| **Sample rate** | **1 MHz** | 104 samples per bit at 9600 baud — ample. Higher rates reduce capture depth |
| **Channels** | D0 (pin 6), D1 (pin 4) | the two data lines |
| **Threshold** | ~1.5 V | for 3.3 V logic |
| **Duration** | 30 s minimum | enough to catch several poll cycles |

At 24 MHz the clone's buffer fills in a fraction of the time. Duration matters
more than resolution here.

## 5. What to capture, in order

| # | Test | What it gives |
|---|---|---|
| 1 | **Idle, 30–60 s** | the steady-state cadence — is the ECM polling, and at what interval? |
| 2 | **Trigger an action** with the app or keypad | which frames carry commands |
| 3 | **Read the module's `/poll` endpoint** right after | correlates bytes with real fireplace state |
| 4 | **Power-cycle the module** | the startup handshake |

### Keep the RF remote's batteries out

The selector may be in **IFT-REM**, so the fireplace can still be commanded even
with the Wi-Fi module removed. Removing the module does not make the unit safe.

## 6. What to look for

A community capture proposed this frame shape:

```
4B 02 4C BC 02 04 A1 B7
│  │  │  │  └─┬──┘ └─┬─┘
│  │  │  │    │      └── MODBUS CRC-16, little-endian
│  │  │  │    └───────── payload
│  │  │  └────────────── command
│  │  └───────────────── device / sequence
└─────────────────────── start byte 0x4B
```

Check for:

- **`0x4B` recurring** as a frame start — the strongest initial signal
- **A sequence counter** incrementing between frames
- **Frame interval** — the poll period
- **CRC-16 over the frame** — try the MODBUS polynomial `0xA001` (reflected),
  stored little-endian
- **Which channel carries which direction** — pin 6 is the ECM's transmit line

The decoded result of this project's captures is in
[FRAME_FORMAT.md](FRAME_FORMAT.md) and [PROTOCOL_DECODED.md](PROTOCOL_DECODED.md).

## 7. Diagnosing module resets

If the stock module resets while you are capturing, watch whether the **+5 V rail
on pin 3** drops at the same moment:

| Observation | Meaning |
|---|---|
| **Rail holds; module restarts** | the module is resetting on its own |
| **Rail drops, then the module restarts** | the ECM may be brown-out resetting the module — a different diagnosis |

Capturing pin 3 alongside the data lines answers this directly, but only if the
analyzer's input rating comfortably covers 5.07 V. **Check that first.** A safer
alternative is a separate DMM on the rail while the analyzer watches the data
lines.

## 8. Safety

1. **Analyzer ground first, removed last.**
2. **Never clip the supply pin (3).**
3. **Analyzer powered from laptop USB only.**
4. **Return the selector to OFF when you walk away** — in IFT-REM the fireplace
   can ignite.
5. **Mains is live in the cavity** — work on the ECM low-voltage side only.
6. **No `tx_pin` anywhere.** This is a listen-only capture.

## Reference

- [ECM_PORT_PINOUT.md](ECM_PORT_PINOUT.md) — measured readings and the
  ground-confirmation method
- [FRAME_FORMAT.md](FRAME_FORMAT.md) — the decoded wire format
