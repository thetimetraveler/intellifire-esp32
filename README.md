# intellifire-esp32

An ESP32/ESPHome replacement for the **IntelliFire Wi-Fi Module (WFM)** used on
Heat & Glo appliances. The board plugs into the ECM's 8-pin Wi-Fi port, speaks
the ECM's 9600-baud polled-slave protocol, and exposes flame, pilot, and flame
height to Home Assistant over the native API. It also publishes a diagnostic
telemetry stream over MQTT.

> **This is a DIY project, not a certified appliance control.** It works on the
> low-voltage side of a gas fireplace. Read [SAFETY.md](SAFETY.md) before
> connecting anything. The ECM owns ignition, flame sensing, and the gas valve;
> this firmware is a control path, never a safety device.

## What it does

- **Replaces the stock WFM** on the ECM's 8-pin port. The stock module contains
  its own STM32 and an AMW136 radio; the ESP32 replaces the whole module and
  answers the ECM directly.
- **Non-optimistic controls.** Flame, pilot, and height report what the ECM says,
  not what was requested. A refused command snaps back to the truth.
- **Remembered height.** Setting height while the flame is off is applied at
  ignition. The ECM reports a synthetic maximum while the burner is off and
  restores its stored height when the flame relights.
- **Bounded retries and a latched fault.** A write is retried every 2 s inside an
  8 s window (four attempts); after that a command fault latches and further
  writes stop until a human clears it.
- **No fail-safe-to-off.** Losing Wi-Fi, the API, or the broker never turns the
  fire off. The wall keypad and RF remote remain the manual fallback.
- **Diagnostics over MQTT.** Every CRC-valid frame seen or sent, a periodic
  opcode census, and an on-demand ring dump, so a fault can be reconstructed
  after the fact.

## Status

The protocol is reverse-engineered and verified against real captures:

| Check | Result |
|---|---|
| Capture replay | **6,145 exchanges compared; 6,136 byte-identical replies; 9 module command injections; 224 unpaired; 0 MISMATCH** |
| Host tests | five C++20 suites pass |
| Python tool tests | selftest, pipeline, and responder suites pass |
| Hardware | bench-verified on an ESP32-S3; an earlier ESP32-C6 build has the longer soak history |

The protocol semantics are only partly known. In particular, the purpose of the
four routine command bytes (`BC`, `DC`, `64`, `6E`) is unresolved, and some status
bytes have no confirmed meaning. The firmware deliberately does not guess at
them.

## Hardware

The current target is an **ESP32-S3** (the reference build uses a Freenove
ESP32-S3 WROOM / ESP32-S3-WROOM-1 N8R8). The earlier ESP32-C6 build is documented
in [docs/wiring-diagram.html](docs/wiring-diagram.html) for reference; the S3
pin map below is authoritative.

### Wiring to the ECM's 8-pin Wi-Fi port

| ECM pin | Signal | ESP32-S3 connection | Notes |
|---|---|---|---|
| 1 | Module-presence gate | **1 kΩ to GND** | The ECM gates power to the port until pin 1 is pulled low. Without this resistor the port stays dark. |
| 2 | Ground (confirmed) | GND | |
| 3 | +5 V supply | 5 V pin through a fuse | Measure the port's current budget before powering from it. **Never USB and pin 3 at the same time.** |
| 4 | Module transmit → ECM receive | **GPIO21 (TX) → 1 kΩ → pin 4** | Pin 4's physical identity is unconfirmed; verify it on your unit. |
| 5 | Idle-high input / possible 3.3 V rail | **Do not connect** | Unresolved. |
| 6 | ECM transmit → module receive | **GPIO14 (RX)** | Input only. Never drive this line. |
| 7 | Not connected | — | |
| 8 | Ground (confirmed) | GND | |

Additional connections:

- **Write-enable sense:** **GPIO1 → GND**. LOW = writes enabled. The component
  requires a pin below 32, and the sense is fail-safe by polarity: an open wire,
  a broken switch, or an unsupported target reads as disabled.
- **Fuse** on the pin-3 feed. The stock module's transmit peak was around
  320 mA; an ESP32 with Wi-Fi peaks around 350 mA. A 0.5–1 A fuse is a design
  band, not a measured value — choose one for your build and record it.
- **Bulk capacitance** across the 5 V rail and ground. Size it for your board's
  transient current and record the value; the reference build's value is not
  documented.
- **Common ground** between the ESP32 and the ECM is required.

Full electrical notes and the C6 diagram: [docs/wiring-diagram.html](docs/wiring-diagram.html).
Port measurements and the ground-confirmation method:
[docs/ECM_PORT_PINOUT.md](docs/ECM_PORT_PINOUT.md).

## Quick start

### 1. Install ESPHome

The reference build used **ESPHome 2026.6.5**. Newer versions should work but
have not been tested.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install esphome==2026.6.5
```

### 2. Create your secrets file

```sh
cp firmware/secrets.yaml.example firmware/secrets.yaml
```

Fill in the six values. Generate the API key with:

```sh
openssl rand -base64 32
```

`firmware/secrets.yaml` is gitignored and must never be committed.

### 3. Validate and flash

```sh
esphome config firmware/fireplace-ecm.yaml
esphome run firmware/fireplace-ecm.yaml
```

The device ships with **no station Wi-Fi credentials**. It starts in AP mode as
`IntelliFire Fallback`; connect to it and use the captive portal to enter your
network. ESPHome persists those credentials to flash, so this is a one-time
setup. The recovery path is the same AP, which matters for a device installed in
a steel cavity.

### 4. Home Assistant

The ESPHome native API exposes the entities directly. MQTT discovery is
deliberately **off**: the API is the entity path, and enabling discovery as well
creates a second set of duplicate entities. The MQTT stream is a diagnostic feed
under `fireplace/ecm/`.

The reference Home Assistant control layer — keypad presses, LED mirroring, a
safety timer, and the pilot/start/stop policy — is described in
[docs/HA_AUTOMATIONS.md](docs/HA_AUTOMATIONS.md). No ready-made YAML is shipped;
that document is a design guide.

## Tests

```sh
make -C firmware test                              # five C++20 host suites
make -C firmware syntax-check                      # component vs. stub headers
make -C firmware test-replay CAPTURES=../captures   # 6,145-exchange replay
make -C firmware python-test                       # Python tool suites
```

The replay target compares the responder against seven recorded captures and
must print `0 MISMATCH`. The Python tests need the files in
`logic_analyzer_data/`; they are included for that reason.

## Repository layout

```
firmware/     ESPHome component, YAML, host tests, Makefile
tools/        capture analysis, replay, harness, and collector tools
docs/         protocol, pinout, board, appliance, analyzer, and HA design docs
captures/     seven frame-pair captures used by the replay test
logic_analyzer_data/  small sigrok/VCD fixtures used by the tests
```

The `firmware/`, `tools/`, `captures/`, and `logic_analyzer_data/` directories
are siblings by design: the Makefile's Python target uses `../tools/...`, and the
responder's selftest globs the capture tree.

## Limitations

- **Not a safety device.** See [SAFETY.md](SAFETY.md).
- **No fan, light, or thermostat control.** The reference ECM reports those
  features absent; the firmware does not implement them.
- **No emulation of the WFM's internal MCU↔radio link.** The ESP32 replaces the
  module and speaks the ECM protocol, not the module's internal protocol.
- **Some protocol fields remain unknown.** Unknown frames are recognised,
  counted, and answered conservatively; they are not interpreted.
- **Pin 4's physical identity is unconfirmed.** Verify it before soldering.
- **The S3 build has less soak history than the earlier C6 build.**

## Documentation

- [SAFETY.md](SAFETY.md) — read first
- [firmware/README.md](firmware/README.md) — firmware behavior, entities, build
- [docs/FRAME_FORMAT.md](docs/FRAME_FORMAT.md) — verified wire format
- [docs/PROTOCOL_DECODED.md](docs/PROTOCOL_DECODED.md) — decoded exchanges
- [docs/COMMAND_MAP.md](docs/COMMAND_MAP.md) — opcodes and confidence levels
- [docs/ECM_PORT_PINOUT.md](docs/ECM_PORT_PINOUT.md) — measured pinout
- [docs/MODULE_BOARD_LAYOUT.md](docs/MODULE_BOARD_LAYOUT.md) — WFM board layout
- [docs/CHIP_IDENTITY.md](docs/CHIP_IDENTITY.md) — AMW136 identification
- [docs/APPLIANCE.md](docs/APPLIANCE.md) — PRIMO / IFT 2.0 facts
- [docs/ANALYZER_SETUP.md](docs/ANALYZER_SETUP.md) — capturing the bus
- [docs/HA_AUTOMATIONS.md](docs/HA_AUTOMATIONS.md) — Home Assistant design

## License

**GPL-3.0**. See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).

The ESPHome C++ runtime is GPLv3 (its Python code is MIT), and the custom
component includes and links that runtime. A GPL-3.0 license for the combined
work is the compatible choice.

## Non-affiliation

This project is not affiliated with, endorsed by, or supported by Hearth & Home
Technologies, Heat & Glo, IntelliFire, Silicon Labs, Espressif, or the ESPHome
project. Product names and trademarks belong to their respective owners. No
vendor firmware, vendor manual, FCC filing image, or decompiled vendor code is
included in this repository.
