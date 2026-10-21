# Radio module identity: AMW136, not AMW006

The radio module on the IntelliFire Wi-Fi module (WFM) is a **Zentri/Silicon Labs
AMW136**. Early notes assumed an AMW006 because the FCC filing is associated with
that family; the physical module is the larger AMW136.

Identification comes from public teardown photographs of the WFM. The module
label shows:

```
AMW136
FCC: 2ABPY-B5BD9
IC: 11685A-B5BD9
```

## The pinout is the same

The AMW036/AMW136 datasheet (ADS-MWx36-101R) and the AMW006/AMW106 datasheet
(ADS-MWx06-109R) carry the same pin table for the signals this project uses:

| Pin | Function | AMW006 | AMW136 |
|---|---|---|---|
| 19 | **UART0_RX** | yes | yes |
| 20 | **UART0_TX** | yes | yes |
| 17 / 18 | UART0_RTS / CTS | yes | yes |
| 25 / 29 | UART1_RX / TX | yes | yes |
| 10 | VDD_3V3 | yes | yes |
| 35 | VDD_WIFI | yes | yes |
| 34 | RESET_N | yes | yes |
| 4 | GPIO_0 / Factory Reset | yes | yes |

Electrical specifications also match: **3.0–3.6 V** (3.3 V typical), **VDD_WIFI
absolute maximum 6.0 V**, **all I/O +5 V tolerant**, and the same current
figures. The pad table and UART assignment are therefore valid for either part.

## The physical size does differ

| Dimension | AMW006 | **AMW136** |
|---|---|---|
| Panel size (`E`) | 20.32 mm | 20.32 mm |
| **Long dimension (`D`)** | **15.24 mm** | **22.86 mm** |
| Height (`A`) | 2.7 mm | **3.40 mm** |
| Inner span (`D1`) | — | 15.24 mm |
| Body (`D2`) | — | 7.62 mm |

The AMW136 is **7.62 mm longer** along one axis and **0.7 mm taller**. Both parts
share the 20.32 mm dimension, so measuring only that dimension identifies the
family, not the part.

Practical consequences:

1. **Measure the long dimension** — about 15.2 mm is an AMW006; about 22.9 mm is
   an AMW136.
2. **Verify the pad pitch on the actual board** rather than assuming 1.27 mm
   transfers between parts.
3. **A carrier or interposer must match 22.86 × 20.32 × 3.40 mm** for the
   AMW136.

## Why the FCC filing says AMW006

FCC ID `2ABPY-B5BD9` appears on both this module and the AMW136 datasheet's
reference module. The filing describes the product family as "Numbat"; both the
AMW006 and AMW136 datasheets use the same footprint figure title. The grant
therefore covers a pin-compatible family, and the FCC ID alone does not say which
member is fitted. That is consistent with the "Contains FCC ID" wording on the
label.

## What to re-verify on hardware

The long dimension (about 22.9 mm expected) and the pad pitch, before any
soldering or interposer work.

## Sources

- [Silicon Labs: Zentri AMW036 / AMW136 data sheet](https://www.silabs.com/documents/public/data-sheets/ADS-MWx36-ZentriOS.pdf)
- Silicon Labs ADS-MWx06-109R, "AMW006 / AMW106" data sheet
- Public WFM teardown photographs in the Home Assistant community forum thread on
  IntelliFire Wi-Fi modules
- FCC ID `2ABPY-B5BD9`
