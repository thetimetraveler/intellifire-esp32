# Appliance identity and control-system facts

The ECM's FCC filing includes the **Heat & Glo PRIMO48/PRIMO48ST/PRIMO60/
PRIMO60ST/PRIMO72/PRIMO72ST Owner's Manual** (9/20). The manual is useful for the
appliance-side facts below, but it covers more than one control-system
generation — read the section heading before using a value.

## Appliance

- **Brand/model family:** Heat & Glo PRIMO — 48 in, 60 in, and 72 in direct-vent
  gas fireplaces.
- Manufacturing dates in the manual: PRIMO48 **from April 2015, active**.
- Furnished with the **IntelliFire Touch** ignition system.

## Control system — read the headings

The manual refers to **"IFT-ECM 2.0"** in its ignition and troubleshooting
sections, but not literally throughout. Pages 26–27 carry legacy **"IntelliFire
Plus"** content (`IPI module`, `8K-1 locks out with 3 LED alarm`), and page 21
says "Inspect IntelliFire Plus flame sensing rod". The manual therefore covers
both generations side by side:

| Evidence | Text |
|---|---|
| Selector switch | "Verify IFT-ECM 2.0 slider switch is in **ON or IFT-REM** position" |
| Error codes | "ECM 2.0 LED Error Codes" |
| Power vent | "check wiring to **IFT-PACM 2.0**" |
| Do-not | "DO NOT cycle the ON/OFF/REM selector" |
| Legacy content | "IntelliFire Plus System", "IPI module", "8K-1 locks out" on pp.26–27 |

The ECM also has multiple hardware revisions. The FCC internal photographs show
`HHT-0201 MAIN BOARD REV B`; an earlier part is `HHT-021`. Part numbers matter
when ordering.

## Power

| Item | Value | Source |
|---|---|---|
| AC/DC adaptor output | **5.7–6.3 Vdc** | troubleshooting matrix |
| Wall transformer | **`SRV2326-131`**, "Transformer, 6 Volt Wall" | service parts list |
| Backup battery pack | must be **> 4.2 V** | troubleshooting matrix |
| Valid selector positions | **ON** or **IFT-REM** (plus OFF) | troubleshooting matrix |

A replacement supply must deliver **5.7–6.3 V**. Battery back-up mode implies the
ECM has a boost path for its 5 V rail that an ESP32 replacement would need to
handle or deliberately skip. **That boost path is an inference, not a measured
fact** — it follows from the battery pack being usable below 5 V, but has not
been verified.

## The safety shutoff

The manual says:

> "The appliance has a safety feature that automatically shuts down the fireplace
> after **nine hours** of continuous operation without receiving a command from
> the remote."

**Treat the nine-hour figure as probable, not confirmed.** It is sourced from an
owner's manual that may not describe the specific ECM firmware revision fitted.
Verify it on your unit.

The reset semantics are the more important question:

- If the ECM counts **any traffic from the module**, then the ESP32's own polls
  may reset the counter. The only protection left would be the ESP32's own timer
  — a single point of failure in DIY software on a gas appliance.
- If it counts **only user commands**, the ESP32 may not need to reimplement
  anything.
- **"From the remote" is ambiguous:** the system has an RF handheld, a wall
  switch, and the Wi-Fi app. The watchdog may count any channel or only the RF
  remote.

Do not rely on the ECM's timer alone. The reference Home Assistant layer runs a
shorter independent timer (see [HA_AUTOMATIONS.md](HA_AUTOMATIONS.md)).

## Other operational facts

- **Safety limit switch** forces shutdown if temperature exceeds specification
  (separate from the nine-hour timer).
- **Spark/ignition:** pilot gap 0.095–0.135 in (2.41–3.43 mm). Ignition attempt
  about 90 s, then lockout.
- **Gas valve solenoids** (diagnostic reference values):
  - pilot solenoid nominal **40 Ω**; kick V > 1 V, hold V minimum **0.26 V**
  - main valve solenoid nominal **60 Ω**
- **Flame sense:** sense-lead resistance must be **< 1 Ω**; sense rod to pilot
  hood **> 1 MΩ** when off.
- **Lockout error codes** (ECM 2.0 LED): 3 Red/1 Green, 2 Red/1 Green, 2 Red/2
  Green (spark coil — replace ECM), 5 Red/1 Green (power vent).
- **`S` = white sense wire**, **`I` = orange ignitor wire** — both land on
  IFT-ECM 2.0 terminals.
- **6-pin valve harness:** black = system ground (must be grounded to chassis),
  orange = pilot solenoid, green = main valve solenoid, brown + black = PV
  pressure switch.

## Sources

- Heat & Glo PRIMO48/PRIMO60/PRIMO72 Owner's Manual (9/20), included in the
  ECM's FCC filing.
- Heat & Glo IFT 2.0 field service guide (`2564_960_IntelliFire_Touch_Controls_2-0_Field_Service_Guide.pdf`),
  available from Heat & Glo's installer documentation site.
- The PRIMO **installation** manual, which carries the wiring diagram and
  control-cavity layout.
