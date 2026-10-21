# Documentation

| Document | What it covers |
|---|---|
| [FRAME_FORMAT.md](FRAME_FORMAT.md) | The verified wire format: start byte, fields, CRC-16/MODBUS, baud rate, direction. |
| [PROTOCOL_DECODED.md](PROTOCOL_DECODED.md) | The decoded request/reply structure, the write slot, status fields, and the evidence behind each claim. |
| [COMMAND_MAP.md](COMMAND_MAP.md) | Opcode and slot map with evidence counts and confidence labels. |
| [ECM_PORT_PINOUT.md](ECM_PORT_PINOUT.md) | Measured 8-pin port readings, the ground-confirmation method, and the open questions. |
| [MODULE_BOARD_LAYOUT.md](MODULE_BOARD_LAYOUT.md) | The WFM PCB: radio, MCU, headers, pads, and the case cutout. |
| [CHIP_IDENTITY.md](CHIP_IDENTITY.md) | Why the radio is an AMW136, not an AMW006, and what changes. |
| [ANALYZER_SETUP.md](ANALYZER_SETUP.md) | How to capture the bus safely with sigrok/PulseView. |
| [APPLIANCE.md](APPLIANCE.md) | PRIMO / IFT 2.0 appliance facts, power, and the safety shutoff. |
| [wiring-diagram.html](wiring-diagram.html) | The earlier ESP32-C6 build's wiring diagram, with the S3 pin map called out. |
| [esp32-ecm-architecture.html](esp32-ecm-architecture.html) | An illustrated architecture walkthrough of the bridge. |

## Confidence labels

The protocol documents use these labels:

- **confirmed** — measured or reproduced, with the evidence stated.
- **strong** — multiple consistent observations, not yet independently verified.
- **likely / plausible** — a hypothesis consistent with the data.
- **unconfirmed / unknown** — do not build on it without a new measurement.

Where a document says "do not guess", it means the value has no verified
meaning. The firmware leaves those fields uninterpreted on purpose.

## What is deliberately not published

This repository contains no vendor firmware image, no decompiled vendor code, no
vendor manual or FCC filing document, no scraped forum content, and no personal
project log. The protocol documents describe observations and measurements made
on the low-voltage port. Vendor documentation is linked, not bundled.

## Scope of the protocol documents

The wire captures show what the ECM and the stock module sent. They do not
describe the ECM's internal firmware. Claims about internal behavior are marked
as inferred or are omitted.
