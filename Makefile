VERSION := 1.1.0
CXX := clang++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fvisibility=hidden -fvisibility-inlines-hidden -I.
BUILD := build

PLUGIN_SRCS := adbfsplugin.cpp adbhandler.cpp wfxcompat.cpp
UNIT_SRCS := tests/test_main.cpp tests/test_globals.cpp wfxcompat.cpp adbhandler.cpp
HDRS := platform.h wfxcompat.h adbfsplugin.h adbhandler.h sdk/common.h sdk/wfxplugin.h

.PHONY: all test clean universal dist

all: $(BUILD)/adbfsplugin.wfx

$(BUILD):
	mkdir -p $@

$(BUILD)/adbfsplugin.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -dynamiclib -o $@ $(PLUGIN_SRCS)

$(BUILD)/unit_tests: $(UNIT_SRCS) tests/harness.h $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(UNIT_SRCS)

$(BUILD)/dlopen_test: tests/test_dlopen.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/test_dlopen.cpp

INTEG_SRCS := tests/test_integration.cpp tests/fake_adb_server.cpp $(PLUGIN_SRCS)

$(BUILD)/integration_tests: $(INTEG_SRCS) $(HDRS) tests/harness.h tests/fake_adb_server.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ $(INTEG_SRCS)

test: $(BUILD)/unit_tests $(BUILD)/integration_tests $(BUILD)/dlopen_test $(BUILD)/adbfsplugin.wfx
	$(BUILD)/unit_tests
	$(BUILD)/integration_tests
	$(BUILD)/dlopen_test $(BUILD)/adbfsplugin.wfx

$(BUILD)/adbfsplugin-universal.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -arch arm64 -arch x86_64 -dynamiclib -o $@ $(PLUGIN_SRCS)

universal: $(BUILD)/adbfsplugin-universal.wfx

dist: test $(BUILD)/adbfsplugin-universal.wfx $(BUILD)/dlopen_test
	$(BUILD)/dlopen_test $(BUILD)/adbfsplugin-universal.wfx
	rm -rf $(BUILD)/dist-stage dist
	mkdir -p $(BUILD)/dist-stage dist
	cp $(BUILD)/adbfsplugin-universal.wfx $(BUILD)/dist-stage/adbfsplugin.wfx
	cp pluginst.inf README.md LICENCE $(BUILD)/dist-stage/
	cd $(BUILD)/dist-stage && zip -q -r ../../dist/adbfsplugin-$(VERSION)-macos.zip .
	@echo "Release: dist/adbfsplugin-$(VERSION)-macos.zip"

clean:
	rm -rf $(BUILD) dist
