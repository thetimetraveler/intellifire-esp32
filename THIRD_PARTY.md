# Third-party software and content

This repository is licensed under **GPL-3.0** (see [LICENSE](LICENSE)). The
following third-party components and references are used or linked.

## ESPHome

- **ESPHome Python code:** MIT License.
- **ESPHome C++ runtime and headers:** GPLv3.

The custom component in `firmware/components/fireplace_ecm/` includes ESPHome
C++ headers and is built and linked into the ESPHome runtime. The combined work
is therefore distributed under GPL-3.0. ESPHome is not vendored in this
repository; install it from PyPI or its own repository.

- <https://esphome.io/>
- <https://github.com/esphome/esphome>

## sigrok / PulseView

The capture and analysis workflow uses sigrok-cli and, optionally, PulseView.
Both are GPLv3. They are external tools, not vendored here.

- <https://sigrok.org/>

## Home Assistant

The design notes in `docs/HA_AUTOMATIONS.md` are original documentation. Home
Assistant itself is Apache-2.0 and is not vendored.

- <https://www.home-assistant.io/>

## Vendor documentation and trademarks

Vendor manuals, FCC filing documents, datasheets, photographs, and firmware
images are **not** included. The documentation links to public sources and
describes measurements and observations. Product names and trademarks —
including Heat & Glo, IntelliFire, Hearth & Home Technologies, Silicon Labs, and
Espressif — belong to their respective owners. This project is not affiliated
with or endorsed by any of them.

## Capture data

The files under `captures/` are original recordings of the electrical traffic on
the ECM's low-voltage serial port, published to document the protocol and to make
the replay test reproducible. They contain no vendor firmware, no personal data,
and no network identifiers. The files under `logic_analyzer_data/` are small
sigrok/VCD fixtures used by the tests.

## Algorithms

The frame checksum is CRC-16/MODBUS (polynomial `0xA001`, reflected, init
`0xFFFF`), a standard public algorithm. No third-party implementation was
copied.

## Contributions

By contributing, you agree that your contribution is licensed under GPL-3.0 and
that you have the right to submit it. Do not submit vendor firmware, vendor
manuals, decompiled vendor code, or material you do not have permission to
publish.
