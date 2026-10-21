# Security policy

## Reporting a vulnerability

Use GitHub's **private vulnerability reporting** for this repository
(Security → Advisories → Report a vulnerability). Do not open a public issue for
a vulnerability, and do not include credentials, personal data, or home-network
details in a report.

Please include:

- the affected version or commit;
- the component (firmware, tool, or documentation);
- reproduction steps or a proof of concept;
- the impact, including whether the issue can affect the appliance.

## Safety-critical issues

If you find an issue that could cause an unintended flame, a failure to shut
down, or any other unsafe appliance behavior:

1. **Stop using the affected build.**
2. Shut the appliance off at its manual valve/disconnect if necessary.
3. Report the issue privately as above.

Do not test a suspected safety issue on a live appliance.

## Scope

In scope:

- the ESPHome component in `firmware/`;
- the host-testable protocol code in `firmware/include/`;
- the tools in `tools/`;
- the documented protocol behavior.

Out of scope:

- vulnerabilities in ESPHome, Home Assistant, sigrok, or the ESP32 toolchain
  (report those upstream);
- physical attacks that require opening the appliance;
- the stock WFM's own firmware or cloud service.

## Secrets

This repository must never contain a real API encryption key, MQTT password,
Wi-Fi password, OTA password, device serial, MAC address, or LAN address. If you
find one, report it privately; do not paste it into an issue.
