#!/usr/bin/env bash
# Prints what is missing and the exact commands to fix it. It NEVER installs anything, uses sudo,
# or downloads scripts; copy/paste the commands you agree with.
set -u
have() { command -v "$1" >/dev/null 2>&1; }
echo "MTPort bootstrap check (read-only)"
echo "-----------------------------------------"
echo "macOS: $(sw_vers -productVersion 2>/dev/null)   arch: $(uname -m)"
BREW_PREFIX=""
if have brew; then BREW_PREFIX="$(brew --prefix)"; echo "Homebrew: $BREW_PREFIX"; else
  echo "Homebrew: NOT FOUND -> see https://brew.sh (official installer; review it before running)"; fi
missing=()
for t in cmake ninja pkg-config; do have "$t" || missing+=("$t"); done
if have pkg-config; then
  for m in libmtp libusb-1.0; do pkg-config --exists "$m" 2>/dev/null || missing+=("$m"); done
else
  [ -n "$BREW_PREFIX" ] && { [ -f "$BREW_PREFIX/include/libmtp.h" ] || missing+=("libmtp"); }
fi
if xcodebuild -version >/dev/null 2>&1; then echo "Xcode: $(xcodebuild -version | head -1)"; else
  echo "Xcode: NOT FOUND (needed only for the SwiftUI app) -> install from the App Store, then: sudo xcode-select -s /Applications/Xcode.app"; fi
have xcodegen && echo "xcodegen: found" || echo "xcodegen: not found (needed to generate the Xcode project for the app): brew install xcodegen"
if [ ${#missing[@]} -gt 0 ]; then
  echo
  echo "Missing build dependencies: ${missing[*]}"
  echo "Install with your consent:"
  echo "    brew install libmtp libusb cmake ninja pkgconf xcodegen"
else
  echo "Build dependencies: all present"
fi
echo
if [ -d /Library/Filesystems/macfuse.fs ]; then
  echo "macFUSE: installed, version $(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null || echo unknown) (optional: only used by 'mtport mount --transport fskit', needs >= 5.4.0)"
else
  echo "macFUSE: not installed (that is fine: the default WebDAV transport does not need it)."
fi
