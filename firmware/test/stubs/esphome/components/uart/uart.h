// Minimal stand-in for ESPHome's uart.h (UARTDevice only).
#pragma once
#include <cstdint>
#include <cstddef>

namespace esphome {
namespace uart {

class UARTComponent {
 public:
  virtual void write_array(const uint8_t *data, size_t len) = 0;
  virtual size_t available() = 0;
  virtual bool read_byte(uint8_t *data) = 0;
};

class UARTDevice {
 public:
  void set_uart_parent(UARTComponent *parent) { this->parent_ = parent; }
  void write_array(const uint8_t *data, size_t len) { this->parent_->write_array(data, len); }
  size_t available() { return this->parent_->available(); }
  bool read_byte(uint8_t *data) { return this->parent_->read_byte(data); }

 protected:
  UARTComponent *parent_{nullptr};
};

}  // namespace uart
}  // namespace esphome
