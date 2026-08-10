VERSION := 1.1.0
CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden -I.
BUILD := build

UNIT_SRCS := tests/test_main.cpp tests/test_globals.cpp wfxcompat.cpp adbhandler.cpp
HDRS := platform.h wfxcompat.h adbfsplugin.h adbhandler.h sdk/common.h sdk/wfxplugin.h

.PHONY: all test clean

all: test

$(BUILD):
	mkdir -p $@

$(BUILD)/unit_tests: $(UNIT_SRCS) tests/harness.h $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(UNIT_SRCS)

test: $(BUILD)/unit_tests
	$(BUILD)/unit_tests

clean:
	rm -rf $(BUILD) dist
