# IFT-WFM module board layout

Reference for the WFM's PCB, derived from public teardown photographs. Connector
positions below were read from the photographs, not from vendor documentation.
Treat the designations as **verified by inspection** and the functions as
**hypotheses** unless stated otherwise.

## Front of the board

```
┌─────────────────────────────────────────────┐
│   ┌───────────────────────────┐             │
│   │  AMW136  radio (shielded) │   C16  J7    │
│   │  label: AMW136 / …B5BD9   │    D7        │
│   │  castellated pads, 4 edges│    D6  R4    │
│   └───────────────────────────┘    D5        │
│                                    R3        │
│   U4  C14 C2                       D4        │
│                                    D3  R2    │
│  ┌──────┐   ┌──────────────────┐   C3        │
│  │ J10  │   │   STM32F411      │   C9  D2    │
│  │ 2×5  │   │   RET7           │   R5  R6    │
│  │ keyed│   │   (U4)           │   D8        │
│  └──────┘   └──────────────────┘             │
│                                              │
│                        □  ○  ○   ← 3 pads    │
└─────────────────────────────────────────────┘
   ^                          ^
   lower-left, left of        bottom-right
   the STM32                  (case cutout)
```

## J10 — 2×5 shrouded keyed header

- **Location:** front of the PCB, lower-left quadrant, silkscreen `J10` directly
  above it; left of and slightly below the STM32F411.
- **Appearance:** 2 rows × 5 pins, shrouded, black plastic, with a polarizing
  key on its left side. Pin 1 is marked by the square silkscreen outline beside
  the key.
- A community teardown noted that this connector does not connect to anything
  when the module is plugged in and is hidden inside the plastic case.

### Is it SWD?

A 2×5 footprint is the conventional Cortex SWD/JTAG shape, so SWD is a
reasonable hypothesis — but neither the community nor the vendor documentation
identifies it as SWD. **Do not assume it.**

To confirm, check continuity: an SWD header would show one pin at GND and
another at VDD_3V3, with low-impedance paths to STM32 pins 46 (SWDIO / PA13) and
49 (SWCLK / PA14) on an LQFP64, plus a likely connection to the RESET net.

If it is SWD and readout protection is not enabled, the module's own MCU could be
inspected directly. That is a research route, not a requirement for this project.

## The three through-hole pads — bottom-right, case cutout

This is the most accessible feature on a module that is still seated in the
appliance.

- **Location:** bottom-right corner of the front PCB.
- **Appearance:** three unpopulated plated through-holes — one square pad on the
  left (the conventional pin-1 marker), then two round pads inside a white
  silkscreen box. A classic 3-pin header footprint.
- A community post asked whether these are TTL; another noted the **cutout in the
  plastic case** that exposes them. Neither was answered with a measurement.

### What they probably are not

The pads sit on the **STM32F411's** side of the board, not the radio's. They are
most likely a **debug or console UART for the module's own MCU** — the internal
link between the MCU and the radio, or a firmware console — rather than the ECM
protocol link.

A community report of an "unintelligible" byte stream on these pins is consistent
with a boot log or debug channel. Measure the idle voltages before drawing
conclusions.

## Other identified features

| Ref | What | Notes |
|---|---|---|
| **AMW136** | The radio module | Shielded, castellated pads on all four edges. Label carries `AMW136`, FCC ID `2ABPY-B5BD9`, and IC `11685A-B5BD9` |
| **U4** | **STM32F411RET7** | LQFP64, marked `ARM` / `STM32F411` / `RET7`. The module's own MCU |
| **J7** | 15-pad single-row | Silkscreen `J7` top-right; a row of gold through-hole pads down the right board edge |
| **GH27G** | SOT-223 regulator | On the **back** of the PCB; steps the ~6 V supply down to 3.3 V for the STM32 and radio. Continuity from its ground tab or input pin to the ECM connector identifies GND and +6 V on the connector immediately |
| Back silkscreen | Vendor copyright notice (2019), `Rev 2.0e`, `UL WM03` | Board identification |

## What this means for a tap

1. **The 3-pad cutout is worth checking first** — it may be the lowest-risk
   access route. Establish what the pads are (measure idle voltages) before
   assuming they carry the ECM protocol.
2. **J10 is a hypothesis, not a finding.** Pursue it only if continuity says SWD
   and readout protection is open.
3. **`GH27G` continuity is the cheap win** — it identifies GND and the ~6 V rail
   on the ECM connector without probing every pin.
4. **The ECM's 8-pin port pin functions remain the core unknown.** None of the
   above substitutes for measuring that port.

## Still not established

- Connector type, pitch, gender, or mating part number for the 8-pin ECM
  interface
- A completed live trace of the module ↔ ECM link from a third party
- Whether the three pads are TTL, what voltage they use, and what protocol they
  carry
