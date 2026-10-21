# Tools

Analysis, capture, replay, and telemetry tools. They are independent of the
firmware build and run on a laptop.

## Requirements

- **Python 3.10+**
- **sigrok-cli** for `.sr` and VCD decoding (`brew install sigrok-cli`, or your
  package manager)
- **paho-mqtt** only for the live mode of `ecm_collector.py`

## Read-only and listen-only guarantees

- `ecm_analyse.py`, `ecm_frames.py`, `ecm_diff.py`, `ecm_pipeline_test.py`,
  `ecm_selftest.py`, and `ecm_responder.py` operate on files. They never touch
  hardware.
- `ecm_capture.sh`, `ecm_harness.py`, and `ecm_monitor.py` capture the bus and
  read the stock module's `/poll` endpoint. They are **listen-only**: they never
  transmit on the ECM link and never call `/post` (the stock module's write
  path).
- `ecm_harness.py` refuses to run without an explicit confirmation and reminds
  the operator to remove the RF remote's batteries.

## Addresses

Examples use `fireplace.local` for the ESP32 and the documentation address
`192.0.2.10` for the stock module or a broker. Replace them with your own
addresses. Do not commit a real LAN address.

## Tool map

| Tool | Purpose |
|---|---|
| `ecm_analyse.py` | Decode a sigrok `.sr` capture and validate MODBUS CRC-16 frames. |
| `ecm_frames.py` | Frame parser and D0/D1 pairing; decodes VCD, `.sr`, and text dumps. |
| `ecm_responder.py` | Replays recorded ECM requests and checks the module's replies; has a selftest. |
| `ecm_selftest.py` | Golden-fixture self-test for the parser and pairing code. |
| `ecm_pipeline_test.py` | End-to-end test from capture layout through `ecm_diff`. |
| `ecm_diff.py` | Compare runs and attribute payload changes to marks/actions. |
| `ecm_harness.py` | Orchestrates a capture with timed marks and `/poll` samples. |
| `ecm_capture.sh` | Small sigrok-cli capture wrapper. |
| `uart_from_csv.py` | Decode a CSV logic-analyzer export when sigrok is unavailable. |
| `ecm_collector.py` | Consume the firmware's MQTT telemetry into run directories. |
| `ecm_monitor.py` | Long segmented monitor with `/poll` sampling. |
| `ecm_session.py` | Session-level analysis used by the collector. |
| `ecm_sweep_matrix.json` | Data-only experiment matrix for a protocol sweep. |

## Common commands

```sh
# Validate the parser against the bundled golden fixture
python3 tools/ecm_selftest.py

# Replay the module's recorded replies
python3 tools/ecm_responder.py --selftest

# Decode a capture
python3 tools/ecm_frames.py logic_analyzer_data/dump3.vcd

# Capture 30 s on two channels
./tools/ecm_capture.sh 30

# Harness a run against the stock module's /poll endpoint
python3 tools/ecm_harness.py --host 192.0.2.10 --duration 60 --yes

# Collect the firmware's MQTT telemetry
python3 tools/ecm_collector.py --broker 192.0.2.10 --out captures/live
```

The Makefile targets in `firmware/` are the easiest way to run the complete test
set:

```sh
make -C firmware python-test
```

## Notes

- `ecm_analyse.py` and `ecm_frames.py` call `sigrok-cli` with an absolute path
  because its wrapper changes directory into its own bundle.
- The sigrok protocol-decoder flag is `-P` / `--protocol-decoders` (plural).
- A bare integer to `sigrok-cli --time` means milliseconds; pass a unit.
- `ecm_harness.py` defaults to a 5 s `/poll` cadence because faster polling can
  disturb the bus. Do not lower it without evidence.
