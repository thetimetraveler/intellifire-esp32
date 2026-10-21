# Safety

**Read this before connecting anything to a fireplace.**

This project controls a **gas appliance**. A mistake can cause a fire, an
explosion, carbon-monoxide poisoning, property damage, or death. It is a DIY
research project, not a certified product.

## The appliance is not yours to redesign

- **Mains voltage is present in the appliance cavity.** The ECM's 8-pin Wi-Fi port
  is on the low-voltage side, but the cavity also contains the 120 V feed and the
  ignition system. Work only on the low-voltage side, and only with the
  appliance's power and gas isolated as required by the manufacturer's
  instructions.
- **The ECM owns ignition, flame sensing, and the gas valve.** This firmware sends
  requests over the same port the stock module used. It cannot bypass the ECM's
  interlocks, and it must not try.
- **This is not a safety device.** It has no flame-proving, no gas-valve control,
  no over-temperature protection, and no certification. The fireplace's own
  controls, limit switches, and venting remain responsible for safe operation.
- **Professional installation and local codes apply.** If you are not qualified to
  work on gas appliances, have the work done by someone who is.
- **Keep a working CO alarm** on every level of the home, and test it regularly.

## No fail-safe-to-off

The firmware deliberately **does not turn the fire off when the network, Home
Assistant, or the ESP32 fails**. A dead ESP32 stops answering the ECM's polls; the
ECM may keep the appliance in its current state. The ECM's own nine-hour safety
deadline is a backstop, not a substitute for supervision.

The wall switch, the RF remote, and the appliance's manual shutoff remain the
real fallbacks. Know where they are before you power the replacement.

## Electrical rules

1. **Never connect USB and ECM pin 3 at the same time.** On the reference board
   the 5 V pin and USB VBUS are the same node; two sources on one net can
   backfeed the laptop and the ECM.
2. **Never probe or clip ECM pin 3 with a logic analyzer.** It is the 5 V supply
   rail, not a signal. The reference analyzer is rated 0–5.25 V with no
   overvoltage protection, and pin 3 measured 5.07 V.
3. **Never drive ECM pin 6.** It is the ECM's transmit line. It is an input to
   the replacement module.
4. **Fit a fuse on the pin-3 feed.** The stock module's transmit peak was around
   320 mA; an ESP32 with Wi-Fi peaks around 350 mA. Choose a fuse for your build
   and record its type and rating.
5. **Fit bulk capacitance across the 5 V rail and ground.** Size it for your
   board's transient current and record the value.
6. **Tie the grounds together.** The ESP32 and the ECM must share a ground.
7. **Pull ECM pin 1 low through 1 kΩ to GND.** The ECM gates power to the Wi-Fi
   port until pin 1 is low. Without the resistor the port stays dark.
8. **Use 3.3 V logic.** The ESP32-S3 is not 5 V tolerant. The measured data lines
   are 3.3 V.
9. **Before powering from the ECM, measure the port's current budget.** The
   reference build did not record a DC-current measurement. Do not assume the
   port can supply your board.

## Bench testing

- **Remove the RF remote's batteries** while probing or capturing. In the
  IFT-REM selector position the fireplace can still be commanded.
- **Return the selector to OFF when you walk away.**
- **Do not leave probes attached unattended.**
- **Use a listen-only capture.** Do not add a transmit line to an analyzer setup.
- **Never connect a logic analyzer to the fireplace's mains side.**

## If something goes wrong

- **Smell gas:** do not operate electrical switches. Leave the building and call
  the gas supplier or emergency services.
- **Unexpected flame, smoke, or noise:** shut off the appliance at its manual
  valve/disconnect and call a qualified service technician.
- **The ESP32 reboots, hangs, or stops answering:** the appliance may remain in
  its last state. Use the wall switch or RF remote; do not rely on the ESP32.

## Warranty and liability

Installing a non-vendor module may void the appliance's warranty and may violate
local codes. The authors of this project provide it **as-is, without warranty of
any kind**, and accept no liability for damage, injury, or death arising from its
use. See [LICENSE](LICENSE).

If you are unsure about any step, stop and consult a licensed professional.
