# Home Assistant control layer — design notes

This document describes the reference Home Assistant layer around the ESPHome
bridge. It is a **design guide, not ready-made YAML**: no automation or script
files are shipped in this repository. Implement the objects below in your own
configuration, using your own entity IDs and notification targets.

The bridge itself is documented in [../firmware/README.md](../firmware/README.md);
the ECM protocol in [PROTOCOL_DECODED.md](PROTOCOL_DECODED.md).

## Design rule: one owner per responsibility

| Object | Owns |
|---|---|
| `automation.fireplace_keypad_control` | What a keypad or SeeTouch Fireplace press means; hands off to scripts. |
| `automation.fireplace_keypad_leds` | **The only writer of the keypad LEDs.** Mirrors the flame sensor. |
| `automation.fireplace_state_sync_safety_timer` | Safety timer, pilot lifecycle, bridge-offline warning, HA-start/periodic reconcile. |
| `script.fireplace_turn_on` | Bridge checks → height 1 → pilot → flame, each confirmed from the ECM. |
| `script.fireplace_turn_off` | Verified shutoff (3 attempts, critical escalation). |
| `script.fireplace_report_problem` | TTS + phone + optional persistent notification. Never writes devices. |
| `script.fireplace_pilot_auto_off` | One idempotent pilot-off sequence for a pilot that HA lit. |

No script writes the keypad LEDs. The safety automation starts the safety timer;
the shutoff script cancels it on a confirmed off as a safety net. Keeping one
writer per responsibility avoids optimistic LED writes that can lie during a
failed ignition.

## Entities that matter

These are the bridge's canonical entity IDs (`name_add_mac_suffix: false`, so no
MAC suffix is added). If you change the friendly names in the YAML, adjust these.

| Entity | Role |
|---|---|
| `binary_sensor.intellifire_flame` | **Truth.** The ECM's own flame report, read back by the bridge. |
| `binary_sensor.intellifire_pilot_light_on` | The ECM's pilot report. |
| `switch.intellifire_flame` | Flame command (non-optimistic). |
| `switch.intellifire_pilot_light` | Pilot command (non-optimistic). |
| `number.intellifire_flame_control` | Flame height, app scale **1–5** (wire/internal is 0–4). |
| `sensor.intellifire_flame_height` | The height the bridge reports; reads **5** while off because the ECM resets its stored height to maximum. |
| `binary_sensor.intellifire_command_fault` | Latched after 4 failed write attempts; blocks new writes until cleared. |
| `button.intellifire_clear_command_fault` | Clears the latch. |
| `binary_sensor.intellifire_ecm_offline_error` | No CRC-valid ECM frame for 5 s. |
| `binary_sensor.intellifire_write_enable` | GPIO sense; LOW = writes enabled. |
| `timer.fireplace_safety_timer` | Safety timer, 4 h, `restore: true`. |
| `input_boolean.fireplace_pilot_auto_lit` | True only when HA lit the pilot; gates the pilot auto-off. |

The `timer` and `input_boolean` are helpers you create in Home Assistant; they
are not part of the ESPHome device.

### Keypad LEDs

A Lutron RA3 keypad's button entities are write-only and expose no `event.*`
entities, so the keypad automation must use **device triggers**. Select your
keypad's Fireplace button in the automation editor and record the generated
`device_id` and `subtype`. Use `switch.<your_keypad>_fireplace_led` for the LED.

If a keypad was reprogrammed after Home Assistant discovered it, its entity slugs
may not match its current button names. Check the friendly name on each LED
entity and identify the **real Fireplace LED** by the button it belongs to, not
by the slug alone. A SeeTouch can have both a `..._keypad_fireplace_led` (the
Front button) and a differently named LED that is actually the Fireplace button.

## Start sequence (`script.fireplace_turn_on`)

1. Wait up to 10 s for the bridge to report a known flame state; report and stop
   if it is offline.
2. If the flame is already on, stop.
3. Refuse if `ecm_offline_error` is on or `write_enable` is off.
4. Clear a latched command fault (5 s); report and stop if it will not clear.
5. **Set height 1.** This is a remembered request: the ESP32 applies it once the
   burner is burning. A height set while the flame is off is not lost.
6. If the pilot is off: claim `input_boolean.fireplace_pilot_auto_lit` **before**
   commanding the pilot, then turn it on and wait up to 20 s for the ECM to
   confirm. Claiming first means a cancelled start cannot leave a pilot on
   untracked. A failure releases the claim only if the pilot is confirmed off; if
   it is on but faulted, the claim is kept so the auto-off can retry.
7. Turn the flame on, wait up to 25 s for the ECM to confirm, settle 5 s, and
   re-check flame and fault.
8. The safety automation starts the timer on the flame-on transition; the LED
   automation lights the LEDs. The start script does neither.

The ESP32 does **not** enforce pilot-before-flame; this script is the policy
owner.

## Stop sequence (`script.fireplace_turn_off`)

- Wait up to 60 s for the bridge if it is rebooting.
- Up to 3 attempts: clear a fault if latched, send off, wait up to 20 s for
  flame-off or fault, settle 5 s.
- If confirmed off: cancel the safety timer (a safety net for a missed
  flame-off trigger) and stop. Use a reason variable so a safety expiry speaks
  the announcement and a keypad press stays silent.
- If not confirmed after 3 attempts: critical TTS, phone notification, and
  persistent notification.
- The safety automation also cancels the timer 10 s after flame-off; the LED
  automation turns the LEDs off as soon as the flame report goes off.

## LED rule (`automation.fireplace_keypad_leds`)

- Triggers: any change of `binary_sensor.intellifire_flame`; HA start; either LED
  returning from `unavailable` (5 s settle); every 30 min.
- Conditions: the flame state is known (`on`/`off`), and at least one LED
  disagrees with it.
- Action: native `if` flame on → both LEDs on, else both off.
- When the flame state is `unavailable`, do nothing. The LEDs keep their last
  known state and are corrected on the next known flame state.
- The idempotency condition means a 30-minute watchdog run with correct LEDs
  sends **no** Lutron commands.

## Keypad press semantics

- Lighting in progress → cancel the start and turn the fire off.
- Shutdown in progress → ignore.
- Flame state `unavailable`/`unknown` → **fail safe to off**; never try to light
  a fire whose state is unknown. If the bridge is unreachable, the shutoff script
  reports critically after its wait.
- Flame on → turn off.
- Flame off → turn on (pilot and height 1 are handled by the start script).

## Pilot and safety policy

- A pilot is lit only when a start needs it. If HA lit it,
  `input_boolean.fireplace_pilot_auto_lit` is set; 30 minutes after the flame
  goes out (or at the periodic reconcile), HA turns it off.
- A pilot turned on by hand is never touched by HA (the flag is off).
- The safety timer is the earlier of the two shutoffs: the ECM has its own
  nine-hour deadline, and `0xBC` polls do not reset it. HA's shorter timer remains
  the real protection. See [APPLIANCE.md](APPLIANCE.md).
- Timer expiry runs the shutoff only if the flame is **not confirmed off**. A
  fire that is already out does not get a false "turned off after 4 hours"
  announcement.
- Bridge-offline detection treats both `unavailable` and `unknown` as offline.
  ESPHome reports `unknown` when the ECM link is lost while the ESP32 is still
  connected.
- The shutoff script is deliberately conservative: 3 attempts, 5 s settle,
  critical escalation. Do not "simplify" away the re-check; a late ignition after
  a cancel is the failure it catches.

## Verification

Static:

- Read each object back after editing.
- Search the configuration for each LED entity and confirm only
  `automation.fireplace_keypad_leds` matches.
- Evaluate the idempotency template with Developer Tools → Template: false when
  the LEDs match the flame, true when they do not.

Live:

1. Turn the pilot off. Run the start script. Expect pilot on, flame on, height 1,
   LEDs on, timer active, no fault.
2. Run the stop script with reason=keypad. Expect flame off, LEDs off, timer idle
   after 10 s, pilot left on, and the pilot flag still set.
3. Run the pilot auto-off script. Expect pilot off and the flag cleared.
4. Manually run the LED automation with correct LEDs. Expect
   `failed_conditions` and no LED state change.
5. With the fire off, start the safety timer for 5 s. When it finishes, confirm
   the automation trace ran but the shutoff script was **not** called.
6. Press each keypad's Fireplace button; confirm the automation trace and the
   correct LED moves.

## Rollback

Keep a backup of each automation and script before editing. Restore the specific
object, or recreate it from the captured configuration. The LED automation and
the pilot auto-off script can simply be deleted if the pre-existing design must
be restored.
