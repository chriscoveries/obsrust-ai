# obs-live-captions: out-of-tree OBS plugin (macOS arm64).
# Builds libstt_live.dylib (Swift SpeechAnalyzer shim) + the C filter against
# the existing fork build of libobs, assembles obs-live-captions.plugin, and
# `make install` drops it into OBS's user plugin dir.

OBS_SRC    ?= $(HOME)/obsrust/obs-studio
LIBOBS_FW  ?= $(OBS_SRC)/build_macos_rust/libobs/RelWithDebInfo/libobs.framework
PLUGIN_DIR  = $(HOME)/Library/Application Support/obs-studio/plugins
BUNDLE      = build/obs-live-captions.plugin

CFLAGS = -O2 -Wall -Wextra \
	-target arm64-apple-macos14.0 \
	-I$(OBS_SRC)/libobs -I$(OBS_SRC)/.deps/obs-deps-2026-08-26-universal/include -I$(OBS_SRC)/build_macos_rust/config \
	-F$(dir $(LIBOBS_FW))

all: bundle

libstt_live.dylib: stt_live.swift
	xcrun swiftc -parse-as-library -swift-version 6 \
		-module-cache-path .cache/swift -Xcc -fmodules-cache-path=.cache/clang \
		-target arm64-apple-macos27.0 -emit-library -O \
		$< -o $@

build/obs-live-captions: live_captions.c | build
	xcrun clang $(CFLAGS) -shared -undefined dynamic_lookup \
		-o $@ $<

build:
	mkdir -p build

bundle: libstt_live.dylib build/obs-live-captions
	mkdir -p "$(BUNDLE)/Contents/MacOS" "$(BUNDLE)/Contents/Resources"
	cp build/obs-live-captions "$(BUNDLE)/Contents/MacOS/obs-live-captions"
	cp libstt_live.dylib "$(BUNDLE)/Contents/Resources/libstt_live.dylib"
	@printf '<?xml version="1.0" encoding="UTF-8"?>\n\
	<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n\
	<plist version="1.0"><dict>\n\
	<key>CFBundleIdentifier</key><string>com.obsproject.obs-live-captions</string>\n\
	<key>CFBundleName</key><string>obs-live-captions</string>\n\
	<key>CFBundleExecutable</key><string>obs-live-captions</string>\n\
	<key>CFBundlePackageType</key><string>BAPL</string>\n\
	</dict></plist>\n' > "$(BUNDLE)/Contents/Info.plist"

install: bundle
	mkdir -p "$(PLUGIN_DIR)"
	rm -rf "$(PLUGIN_DIR)/obs-live-captions.plugin"
	cp -R "$(BUNDLE)" "$(PLUGIN_DIR)/obs-live-captions.plugin"
	@echo "installed: $(PLUGIN_DIR)/obs-live-captions.plugin"

# Headless check: dlopen the dylib and feed a WAV's PCM through it.
test-host: build/test-host
	DYLD_LIBRARY_PATH=. ./build/test-host ../obsrust/spikes/2026-10-12/speech-rollup/speech-30s.wav

build/test-host: test_host.c | build
	xcrun clang -O2 -target arm64-apple-macos14.0 -o $@ $< -ldl

clean:
	rm -rf build libstt_live.dylib .cache

.PHONY: all bundle install clean test-host
