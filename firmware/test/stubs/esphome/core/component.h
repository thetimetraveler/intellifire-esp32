// Minimal stand-in for ESPHome's component.h, matching the API surface used by
// fireplace_ecm.{h,cpp}: Component, setup_priority::BUS, millis(), log macros.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>

namespace esphome {

namespace setup_priority {
inline constexpr float BUS = 1000.0f;
inline constexpr float HARDWARE = 800.0f;
}  // namespace setup_priority

uint32_t millis();
void esph_consume_tag_(const char *tag);

class Component {
 public:
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
  virtual ~Component() = default;
};

}  // namespace esphome

#define ESP_LOGE(tag, ...) ((void) (tag))
#define ESP_LOGW(tag, ...) ((void) 0)
#define ESP_LOGI(tag, ...) ((void) 0)
#define ESP_LOGD(tag, ...) ((void) 0)
#define ESP_LOGCONFIG(tag, ...) esph_consume_tag_(tag)
#define ESP_LOGV(tag, ...) ((void) 0)
