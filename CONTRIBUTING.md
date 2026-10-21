# Contributing

Thanks for helping. This project touches a gas appliance, so safety and evidence
come before speed.

## Ground rules

1. **Do not post credentials, personal data, or home-network details** in issues,
   pull requests, commits, or test fixtures. Redact captures before attaching
   them.
2. **Do not submit vendor firmware, vendor manuals, FCC filing documents,
   decompiled vendor code, or material you do not have permission to publish.**
3. **Protocol claims need evidence.** Say whether a claim is measured, observed
   in a capture, inferred, or a hypothesis. Do not present a hypothesis as a
   finding.
4. **Hardware changes need a safety review.** Explain the failure modes,
   including what happens if the ESP32 hangs, the network drops, or a wire opens.
5. **Keep the firmware non-optimistic.** Entity state must come from the ECM's
   status frame, not from what was requested.

## Before opening a pull request

```sh
make -C firmware test
make -C firmware test-replay CAPTURES=../captures
make -C firmware python-test
```

All three must pass, and the replay must report `0 MISMATCH`. If you add or
change a capture, include only the `frames_D0.jsonl` and `frames_D1.jsonl`
files; capture metadata can contain device and network identifiers.

Check that your changes do not introduce:

- a real API key, MQTT password, Wi-Fi credential, serial number, MAC address,
  or LAN address;
- an absolute path from your machine;
- a new write opcode without capture evidence and a safety rationale;
- a path that can turn the fire off when the network or API fails (there is
  deliberately no fail-safe-to-off).

## Style

- Match the surrounding code and comment style.
- Prefer small, focused changes.
- Keep host-testable protocol logic in `firmware/include/` free of ESPHome
  dependencies, so it can be compiled on a laptop.
- Add a host test for every behavior change in the protocol core.

## Licensing

By contributing, you agree that your contribution is licensed under GPL-3.0.
See [LICENSE](LICENSE) and [THIRD_PARTY.md](THIRD_PARTY.md).

## Safety

Read [SAFETY.md](SAFETY.md). If a change could affect ignition, flame sensing, the
gas valve, or the fail-safe behavior of the appliance, describe the risk in the
pull request and do not merge it without review.
