"""ESPHome external component: fireplace_ecm.

The ECM's polled slave, reimplemented. Answers the ECM's 9600-8-N-1 polls on the
UART and carries commands in the 0xBC reply's 4-byte data slot.

The wire format (CRC-16/MODBUS, frame parsing, reply construction, the write
slot) lives in ``firmware/include/ecm_protocol.h`` — pure C++20 with no ESPHome
dependency, so it can be compiled and unit-tested on a host:

    clang++ -std=c++20 -Wall -Wextra -fsyntax-only include/ecm_protocol.h
    clang++ -std=c++20 -Wall -Wextra -o /tmp/ecm_host_test test/ecm_host_test.cpp && /tmp/ecm_host_test
"""

import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["uart"]

CONF_UART_ID = "uart_id"
CONF_WRITE_ENABLE_PIN = "write_enable_pin"

fireplace_ecm_ns = cg.esphome_ns.namespace("fireplace_ecm")
FireplaceECM = fireplace_ecm_ns.class_("FireplaceECM", cg.Component, uart.UARTDevice)

# firmware/include/ is where the host-testable protocol core lives. ESPHome puts
# a component's OWN directory on the include path, not the config directory, so
# add it explicitly (absolute, because the build runs from a different cwd).
INCLUDE_DIR = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "include")
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FireplaceECM),
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        # Required, not optional: an unwired sense pin silently disables every
        # write, and a silent never-writes is much harder to debug than a config
        # error. The polarity (LOW = enabled) is implemented in the component.
        cv.Required(CONF_WRITE_ENABLE_PIN): cv.int_,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_build_flag(f"-I{INCLUDE_DIR}")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_write_enable_pin(config[CONF_WRITE_ENABLE_PIN]))
