// Minimal stand-in for esp-idf's driver/gpio.h (subset used by the component).
#pragma once
#include <cstdint>

typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE = 1 } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE = 1 } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0 } gpio_int_type_t;

typedef struct {
  uint64_t pin_bit_mask;
  gpio_mode_t mode;
  gpio_pullup_t pull_up_en;
  gpio_pulldown_t pull_down_en;
  gpio_int_type_t intr_type;
} gpio_config_t;

int gpio_config(const gpio_config_t *cfg);
int gpio_get_level(gpio_num_t pin);
