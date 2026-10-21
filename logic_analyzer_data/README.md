# Logic-analyzer fixtures

Small fixtures kept so the parser tests and examples run without external files.

| File | What it is |
|---|---|
| `dump3.vcd` | 1 MHz, 2-channel sigrok VCD capture. The golden fixture: its frames are the ones hard-coded in `tools/ecm_selftest.py`. |
| `dump1.sr` | A lower-rate sigrok capture used by the deliberately weak timing check in the selftest. |
| `dec3_D0.txt` | Decoder output for `dump3.vcd`, channel D0, in the text format the parser accepts. |
| `dec3_D1.txt` | Decoder output for `dump3.vcd`, channel D1. |
| `sig_dump3_D0.txt` | sigrok UART-decoder output for `dump3.vcd`, channel D0. |
| `sig_dump3_D1.txt` | sigrok UART-decoder output for `dump3.vcd`, channel D1. |

`dump3.vcd` decodes to four frames on each channel at 9600-8-N-1. On D0 that is
8 + 31 + 8 + 8 bytes (the `BC`, `DC`, `BC`, and `6E` frames); D1 mirrors them
with the module's device offsets and its own payload lengths.

The VCD is a logic-level recording of the low-voltage serial lines. It contains
no network identifiers, serial numbers, or personal data. Stale or corrupt
captures, raw `.sr` recordings, and sweep indexes from the original project are
not included.

## Using a fixture

```sh
python3 tools/ecm_frames.py logic_analyzer_data/dump3.vcd
python3 tools/ecm_selftest.py --vcd logic_analyzer_data/dump3.vcd --sr logic_analyzer_data/dump1.sr
```
