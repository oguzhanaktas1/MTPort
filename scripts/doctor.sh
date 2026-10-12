#!/usr/bin/env bash
# Honest environment report. Distinguishes "package missing" from "phone not plugged in".
# Uses the built CLI for the MTP/macFUSE checks when available.
set -u
cd "$(dirname "$0")/.."
export PATH="$(brew --prefix 2>/dev/null || echo /opt/homebrew)/bin:/usr/local/bin:$PATH"
have() { command -v "$1" >/dev/null 2>&1; }
line() { printf '%-26s %s\n' "$1" "$2"; }

echo "== toolchain"
line "macOS" "$(sw_vers -productVersion) ($(sw_vers -buildVersion)) $(uname -m)"
line "xcode-select" "$(xcode-select -p 2>/dev/null || echo 'NOT SET')"
line "xcodebuild" "$(xcodebuild -version 2>/dev/null | tr '\n' ' ' || echo 'not available')"
for t in cmake ninja pkg-config clang++ xcodegen; do line "$t" "$(command -v $t || echo 'MISSING')"; done
if have pkg-config; then
  line "libmtp (pkg-config)" "$(pkg-config --modversion libmtp 2>/dev/null || echo 'MISSING - brew install libmtp')"
  line "libusb (pkg-config)" "$(pkg-config --modversion libusb-1.0 2>/dev/null || echo 'MISSING - brew install libusb')"
fi
line "mtp-detect" "$(command -v mtp-detect || echo 'not installed (comes with libmtp)')"

echo
echo "== USB (visibility is NOT proof of MTP readiness)"
system_profiler SPUSBDataType 2>/dev/null | grep -E "^\s{4,}[A-Za-z0-9].*:$" | grep -viE "USB (3|2|31)\.[0-9] (Bus|Root)|Hub|Host Controller" | head -20 || true

echo
echo "== MTPort"
CLI=""
for c in build/native/cli/mtport build-release/native/cli/mtport; do [ -x "$c" ] && CLI="$c" && break; done
if [ -n "$CLI" ]; then "$CLI" doctor; else echo "CLI not built yet: scripts/build-native.sh"; fi
