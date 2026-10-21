# Host tests for the IntelliFire ECM bridge.
#
# These build and run on a laptop with no ESP32 attached. They exercise only
# include/*.h, which is deliberately free of ESPHome dependencies.
#
# NOT covered here (see README): components/fireplace_ecm/*.cpp, the MQTT path
# behind #ifdef USE_MQTT, fireplace-ecm.yaml, and the __init__.py schema.

CXX      ?= clang++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O1
BUILD    ?= /tmp/ecm-tests

TESTS := ecm_host_test ecm_reconciler_test ecm_telemetry_test ecm_inventory_test ecm_framing_test
BINS  := $(addprefix $(BUILD)/,$(TESTS))

.PHONY: all test syntax-check test-replay python-test clean

all: $(BINS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%: test/%.cpp include/*.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -Iinclude -o $@ $<

test: all
	@set -e; for t in $(TESTS); do \
		printf '\n== %s ==\n' "$$t"; \
		$(BUILD)/$$t; \
	done

# Compile the component against the stub ESPHome/ESP-IDF headers, on both the
# ESP32 path and the fail-safe (non-ESP32) path. No board or SDK is needed.
syntax-check:
	$(CXX) $(CXXFLAGS) -Wpedantic -DUSE_ESP32 -Itest/stubs -Iinclude -fsyntax-only components/fireplace_ecm/fireplace_ecm.cpp
	$(CXX) $(CXXFLAGS) -Wpedantic -Itest/stubs -Iinclude -fsyntax-only components/fireplace_ecm/fireplace_ecm.cpp

# Full capture replay. Point CAPTURES at the captures directory:
#   make test-replay CAPTURES=../captures
test-replay: $(BUILD)/ecm_host_test
	$(BUILD)/ecm_host_test $(CAPTURES)

python-test:
	@set -e; \
	python3 ../tools/ecm_selftest.py; \
	python3 ../tools/ecm_pipeline_test.py; \
	python3 ../tools/ecm_responder.py --selftest --quiet

clean:
	rm -rf $(BUILD)
