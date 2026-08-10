VERSION := 1.1.0
CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden -I.
BUILD := build

PLUGIN_SRCS := adbfsplugin.cpp adbhandler.cpp wfxcompat.cpp
UNIT_SRCS := tests/test_main.cpp tests/test_globals.cpp wfxcompat.cpp adbhandler.cpp
HDRS := platform.h wfxcompat.h adbfsplugin.h adbhandler.h sdk/common.h sdk/wfxplugin.h

.PHONY: all test clean

all: $(BUILD)/adbfsplugin.wfx

$(BUILD):
	mkdir -p $@

$(BUILD)/adbfsplugin.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -dynamiclib -o $@ $(PLUGIN_SRCS)

$(BUILD)/unit_tests: $(UNIT_SRCS) tests/harness.h $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(UNIT_SRCS)

$(BUILD)/dlopen_test: tests/test_dlopen.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_dlopen.cpp

test: $(BUILD)/unit_tests $(BUILD)/dlopen_test $(BUILD)/adbfsplugin.wfx
	$(BUILD)/unit_tests
	$(BUILD)/dlopen_test $(BUILD)/adbfsplugin.wfx

clean:
	rm -rf $(BUILD) dist
