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

# Double Commander's plugin check (GetPluginBinaryType) only accepts THIN
# Mach-O binaries — never ship a universal/fat .wfx. One zip per architecture.
NATIVE_ARCH := $(shell uname -m)
FOREIGN_ARCH := $(if $(filter arm64,$(NATIVE_ARCH)),x86_64,arm64)

$(BUILD)/thin-arm64/adbfsplugin.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	mkdir -p $(BUILD)/thin-arm64
	$(CXX) $(CXXFLAGS) -arch arm64 -dynamiclib -o $@ $(PLUGIN_SRCS)

$(BUILD)/thin-x86_64/adbfsplugin.wfx: $(PLUGIN_SRCS) $(HDRS) | $(BUILD)
	mkdir -p $(BUILD)/thin-x86_64
	$(CXX) $(CXXFLAGS) -arch x86_64 -dynamiclib -o $@ $(PLUGIN_SRCS)

dist: test $(BUILD)/thin-arm64/adbfsplugin.wfx $(BUILD)/thin-x86_64/adbfsplugin.wfx $(BUILD)/dlopen_test
	$(BUILD)/dlopen_test $(BUILD)/thin-$(NATIVE_ARCH)/adbfsplugin.wfx
	$(BUILD)/dlopen_test --magic-only $(BUILD)/thin-$(FOREIGN_ARCH)/adbfsplugin.wfx
	rm -rf $(BUILD)/dist-stage dist
	mkdir -p dist
	for arch in arm64 x86_64; do \
		mkdir -p $(BUILD)/dist-stage/$$arch && \
		cp $(BUILD)/thin-$$arch/adbfsplugin.wfx $(BUILD)/dist-stage/$$arch/ && \
		cp pluginst.inf README.md LICENCE $(BUILD)/dist-stage/$$arch/ && \
		(cd $(BUILD)/dist-stage/$$arch && zip -q -r ../../../dist/adbfsplugin-$(VERSION)-macos-$$arch.zip .) || exit 1; \
	done
	@echo "Release: dist/adbfsplugin-$(VERSION)-macos-arm64.zip and dist/adbfsplugin-$(VERSION)-macos-x86_64.zip"

clean:
	rm -rf $(BUILD) dist
