// Minimal stand-in for esp-idf's esp_random.h (subset used by the component).
//
// The component draws its telemetry boot id from esp_random() so that a device
// reboot can be told apart from an MQTT reconnect. Only the declaration is
// needed for the host syntax check; the real header comes from ESP-IDF on a
// real build.
#pragma once
#include <cstdint>

uint32_t esp_random(void);
