# fireplace_ecm — ESPHome component

The ESPHome side of the project. It makes an ESP32 answer the IntelliFire ECM's
9600-baud polls and carry flame, pilot, and flame-height commands in the 4-byte
write slot of the `0xBC` reply.

Read the root [README](../README.md) and [SAFETY.md](../SAFETY.md) first.

## What it replaces

The stock Wi-Fi module contains two processors:

1. an **STM32F411** that speaks the ECM's serial protocol on the 8-pin port, and
2. an **AMW136** Wi-Fi radio connected to the STM32.

This firmware replaces the **whole module** and speaks the ECM protocol
directly. It does not emulate the STM32↔radio link. The ECM is a **polled
master**: it opens every exchange, and the module only ever replies. There is no
"send a command" operation in the protocol; commands ride in the reply's data
slot.

The wire format and the observed exchange structure are documented in
[../docs/FRAME_FORMAT.md](../docs/FRAME_FORMAT.md) and
[../docs/PROTOCOL_DECODED.md](../docs/PROTOCOL_DECODED.md).

## Wiring

The current target is an **ESP32-S3**. The reference build uses a Freenove
ESP32-S3 WROOM (ESP32-S3-WROOM-1 N8R8); `esp32-s3-devkitc-1` is the board entry
used in the YAML.

| ECM pin | Signal | ESP32-S3 | Notes |
|---|---|---|---|
| 1 | Module-presence gate | **1 kΩ to GND** | The ECM gates port power until pin 1 is pulled low. Without it the port stays dark. |
| 2 | Ground | GND | Confirmed ground. |
| 3 | +5 V supply | 5 V through a fuse | Measure the port's current budget first. **Never USB and pin 3 together.** |
| 4 | Module TX → ECM RX | **GPIO21 (TX) → 1 kΩ → pin 4** | Pin 4's physical identity is unconfirmed; verify it. |
| 5 | Idle-high input / possible rail | **Do not connect** | Unresolved. |
| 6 | ECM TX → module RX | **GPIO14 (RX)** | Input only. Never drive it. |
| 7 | Not connected | — | |
| 8 | Ground | GND | Confirmed ground. |

- **Write-enable sense:** **GPIO1 → GND**. LOW = writes enabled. The component
  requires a pin below 32, and the polarity is fail-safe: an open wire, a broken
  switch, or an unsupported target reads as disabled.
- **GPIO2 is disqualified** on the reference board (onboard IO2 LED); a divider
  could park a floating sense pin in the indeterminate zone.
- **Avoid on ESP32-S3:** strapping pins 0/3/45/46; native USB 19/20; flash/PSRAM
  26–37; microSD 38–40; UART0 43/44; WS2812 48; camera 4–13/15–18.
- **Bulk capacitance** across 5 V and GND, and a **fuse** on the pin-3 feed, are
  required. Size both for your board and record the values.
- The ECM's serial lines are 3.3 V logic. The ESP32-S3 is not 5 V tolerant.

The earlier ESP32-C6 build and its full wiring diagram are in
[../docs/wiring-diagram.html](../docs/wiring-diagram.html). Port measurements are
in [../docs/ECM_PORT_PINOUT.md](../docs/ECM_PORT_PINOUT.md).

## Behavior

### The polled-slave contract

- The ECM sends `0x02BC` (type 2) asking for four bytes.
- The reply echoes the request's first two payload bytes and carries a
  four-byte write slot. `00 00 00 00` is idle.
- Only slot commands change appliance state. The module never initiates a frame.
- `seq` is an **exchange counter shared by both directions**. Copy it into the
  reply; never predict or validate it. The ECM can reset it to 0 after a UART
  overrun.
- The ECM's power-on frame is command word **`0x0320`**. Match the full word:
  schedule announcement `0x0220` shares the low byte `0x20` and is not a boot.

### Non-optimistic state

Every writable entity reads its state from the ECM's own status frame. A command
the appliance refuses snaps back to the truth instead of lying to Home
Assistant.

### Boot writes nothing

The flame and pilot switches use `restore_mode: DISABLED`. ESPHome's default
restore mode would act on a default state during `setup()` and write an OFF
command to the appliance on every boot. That is explicitly prevented: an ESP32
reboot must never re-command the fireplace.

### Reconciler

- Desired flame, pilot, and height are tracked separately.
- One write per poll, in fixed order: **flame → pilot → height**.
- Height is legal only while the ECM reports the flame on. A height set while the
  flame is off is **remembered and applied at ignition**.
- A write is retried every **2 s**. The command window is **8 s**, which allows at
  most four sends; then a **command fault latches**.
- While faulted, ON requests are not targeted. **OFF requests are still
  honored** — refusing to turn something off is the wrong direction to fail in.
- A latched fault is cleared only by a human, through the
  `button.intellifire_clear_command_fault` entity.

### Height scale

Home Assistant and the app use **1–5**. The wire uses **0–4**; the component
converts. The ECM reports a synthetic maximum (4 on the wire, 5 in the app)
while the burner is off and restores its stored height on flame-on, so
`sensor.intellifire_flame_height` reads **5** while the flame is off. That is
faithful to the reported state, not a bug.

### Pilot

The component does **not** enforce pilot-before-flame. The ECM enforces no such
interlock, and the Home Assistant layer owns the policy. A flame-on while the ECM
reports the pilot off is still written; the outcome is surfaced as
`FlameWithoutPilot` in `text_sensor.intellifire_last_command_outcome`.

### Fail-safe posture

- **No fail-safe-to-off.** Losing Wi-Fi, the API, or the MQTT broker leaves the
  appliance in its current state. This is deliberate: the wall keypad and RF
  remote remain the manual fallback, and a network dropout must not take the
  ECM's only slave down.
- **Reboot timeouts are 0 s** for Wi-Fi, the API, and MQTT. A network outage
  must not reboot the device.
- **Safe mode is disabled.** Safe mode would stop the application components,
  leaving the ECM with a silent slave. Fix the cause of a crash loop instead.
- **The write-enable sense is fail-safe by polarity.** An open or broken sense
  wire reads as disabled.

## Entities

With `friendly_name: IntelliFire` and `name_add_mac_suffix: false`, the entity
IDs are stable:

| Entity | Meaning |
|---|---|
| `switch.intellifire_flame` | Flame request. Non-optimistic. |
| `switch.intellifire_pilot_light` | Pilot request. Non-optimistic. |
| `number.intellifire_flame_control` | Height, app scale 1–5. |
| `sensor.intellifire_flame_height` | Height reported by the ECM (1–5; 5 while off). |
| `sensor.intellifire_uptime` | Device uptime. |
| `sensor.intellifire_reply_latency` | Time from request to reply, ms. |
| `sensor.intellifire_ecm_frames` | CRC-valid frames seen. |
| `sensor.intellifire_crc_errors` | Parse failures plus framing overflows. |
| `sensor.intellifire_unknown_commands` | Frames with an unrecognised command word. |
| `sensor.intellifire_ecm_resets` | ECM boot frames observed. |
| `sensor.intellifire_seq_resets` | Exchange-counter resets to 0. |
| `sensor.intellifire_status_config_byte` | Status byte 6 (meaning unresolved). |
| `sensor.intellifire_heat_demand` | Status byte 8 (meaning unresolved). |
| `sensor.intellifire_frames_replied` | Frames actually answered. Compare with `Ecm Frames`. |
| `sensor.intellifire_records_dropped` | Telemetry records lost because MQTT was down or backed up. |
| `binary_sensor.intellifire_flame` | **Truth:** the ECM's flame report. |
| `binary_sensor.intellifire_pilot_light_on` | The ECM's pilot report. |
| `binary_sensor.intellifire_status_flag_9` | Status byte 9: a 32-bit deadline is armed. Not a heat call. |
| `binary_sensor.intellifire_ecm_offline_error` | No CRC-valid ECM frame for 5 s. |
| `binary_sensor.intellifire_write_enable` | Write-enable sense. LOW = enabled. |
| `binary_sensor.intellifire_command_fault` | Latched after the retry window. |
| `text_sensor.intellifire_last_command_outcome` | Human-readable last result. |
| `button.intellifire_clear_command_fault` | Clears the latch. |
| `button.intellifire_dump_telemetry` | Publishes the recent-history ring to MQTT. |

Entities that could never fire are deliberately absent. In particular, there is
no "new opcode" entity: no genuinely new opcode has been observed, so it would
sit at zero forever. New opcodes are reported over MQTT instead.

There is also no flame-error or pilot-error entity: the ECM's fault codes are
internal and are not transmitted on this link. A stub hardwired to `false` would
be a silently-never-firing alarm, which is worse than no entity.

## Configuration and secrets

The YAML expects `firmware/secrets.yaml` with six values:

| Key | Purpose |
|---|---|
| `ap_password` | Password for the fallback AP. |
| `api_encryption_key` | ESPHome native API encryption key. |
| `ota_password` | OTA password. |
| `mqtt_broker` | Broker for the diagnostic telemetry stream. |
| `mqtt_username` | MQTT username. |
| `mqtt_password` | MQTT password. |

Copy `secrets.yaml.example` and fill it in. `secrets.yaml` is gitignored.

There is **no station Wi-Fi block**. The device comes up in AP mode as
`IntelliFire Fallback`; the captive portal accepts network credentials and
ESPHome persists them to flash. The AP remains the recovery path, which matters
for a device installed in a metal cavity.

Generate the API key with `openssl rand -base64 32`.

### Runtime MQTT override

The `set_mqtt_config` API service changes the broker without editing the YAML or
reflashing. It **reboots the device**: ESPHome's MQTT client has no public
reconnect path. The values are written to flash first, so they survive the
reboot.

## MQTT telemetry

MQTT is a diagnostic stream, not an entity namespace. Discovery is off; Home
Assistant reaches the entities over the native API.

| Topic | Content |
|---|---|
| `fireplace/ecm/status` | `online` / `offline` birth and will messages. |
| `fireplace/ecm/frame` | One JSON record per frame seen or sent. |
| `fireplace/ecm/telemetry/summary` | Retained periodic counters. |
| `fireplace/ecm/telemetry/alert` | New-opcode or census alerts. |
| `fireplace/ecm/telemetry/dump/header`, `.../chunk/<n>`, `.../footer` | On-demand ring dump. |
| `fireplace/ecm/inventory/<slug>` | Retained opcode-census keys. |

`tools/ecm_collector.py` consumes the stream into run directories;
`tools/ecm_diff.py` and the other tools analyse them.

## Build and flash

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install esphome==2026.6.5

cp secrets.yaml.example secrets.yaml
# edit secrets.yaml

esphome config fireplace-ecm.yaml
esphome run fireplace-ecm.yaml
```

The reference build used ESPHome 2026.6.5. Newer versions should work but have
not been tested.

On first boot, connect to `IntelliFire Fallback` and use the captive portal. For
updates, use OTA (`esphome run ... --device <address>` or the ESPHome
dashboard). **Do not connect USB while the board is powered from ECM pin 3.**

## Host tests

The protocol core in `include/` has no ESPHome dependency and compiles on a
laptop:

```sh
make test
make syntax-check
make test-replay CAPTURES=../captures
make python-test
```

- `make test` builds and runs five C++20 suites:
  `ecm_host_test`, `ecm_reconciler_test`, `ecm_telemetry_test`,
  `ecm_inventory_test`, `ecm_framing_test`.
- `make syntax-check` compiles the component against the stub ESPHome/ESP-IDF
  headers in `test/stubs/`, on both the ESP32 path and the fail-safe
  (non-ESP32) path. It needs no board or SDK.
- `make test-replay` replays seven captures. The expected result is
  `compared 6145 exchanges: 6136 byte-identical, 9 module command injections
  (correctly NOT reproduced), 224 unpaired, 0 MISMATCH`.
- `make python-test` runs the Python selftest, pipeline, and responder suites.

The C++ tests exercise only `include/*.h`. They do not cover
`components/fireplace_ecm/*.cpp`, the MQTT path behind `#ifdef USE_MQTT`, the
YAML schema, or the ESPHome build.

## Verified and unverified

Verified:

- Frame format: `0x4B` start, MODBUS CRC-16 little-endian, 9600-8-N-1.
- The polled-slave request/reply shape and the write slot in the `0xBC` reply.
- Write opcodes `0x64` (flame), `0x65` (pilot), `0x67` (height).
- Status bytes 5 (pilot), 7 (height), 9 (deadline flag), 10 (flame).
- Seven-capture replay with zero mismatches.
- The ESP32-S3 build and the host tests.

Not verified or not implemented:

- The purpose of the routine command bytes `BC`, `DC`, `64`, and `6E`.
- The human meaning of several status bytes.
- Pin 4's physical identity (capture evidence supports it; a physical
  confirmation is still required before soldering).
- Long-term soak behavior of the S3 build. The earlier C6 build has more history.
- Fan, light, and thermostat control. The reference ECM reports those features
  absent, and the firmware does not implement them.
- Emulation of the stock module's local `/poll` HTTP API.

## Troubleshooting

| Symptom | Check |
|---|---|
| No entities after flashing | Native API key and network reachability; the device may be in AP mode. |
| All entities `unavailable`, `Ecm Offline Error` on | ECM pin 6 → GPIO14, common ground, and the pin-1 gate resistor. |
| Port is dark (pin 3 at 0 V) | Pin 1 presence gate: 1 kΩ from pin 1 to GND. |
| Writes never leave | `Write Enable` must be on (GPIO1 low). Check the sense wiring. |
| `Command Fault` latched | Read `Last Command Outcome`, fix the cause, then press Clear Command Fault. |
| Height reads 5 while off | Expected: the ECM reports a synthetic maximum while off and restores the stored height on flame-on. |
| Repeated reboots | Check the 5 V supply, fuse, bulk capacitance, and that USB is not connected with pin 3. |

## Safety and license

See [../SAFETY.md](../SAFETY.md). The ECM owns ignition, flame sensing, and the
gas valve; this firmware is a control path, never a safety device.

GPL-3.0. See [../LICENSE](../LICENSE) and [../THIRD_PARTY.md](../THIRD_PARTY.md).
