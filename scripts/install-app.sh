#!/usr/bin/env bash
# Builds MTPort from source and installs MTPort.app into ~/Applications (no sudo, no downloads).
# usage: scripts/install-app.sh [--no-open]      (MTPORT_INSTALL_DIR=/some/dir installs there instead of ~/Applications)
# Needs: Xcode (for the SwiftUI app) and the Homebrew packages listed by scripts/bootstrap.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$(brew --prefix 2>/dev/null || echo /opt/homebrew)/bin:/usr/local/bin:$PATH"

OPEN=1
[ "${1:-}" = "--no-open" ] && OPEN=0
for t in cmake ninja pkg-config xcodegen xcodebuild; do
  command -v "$t" >/dev/null || { echo "missing '$t' - run scripts/bootstrap.sh to see what to install" >&2; exit 1; }
done

echo "==> Building the native tools (Release)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

echo "==> Building the app"
( cd macos/MTPort \
  && xcodegen generate \
  && xcodebuild -project MTPort.xcodeproj -scheme MTPort -configuration Release \
       -derivedDataPath ../../build/xcode build | tail -n 3 )
APP="build/xcode/Build/Products/Release/MTPort.app"
[ -d "$APP" ] || { echo "build failed: $APP not found" >&2; exit 1; }

DEST="${MTPORT_INSTALL_DIR:-$HOME/Applications}"
mkdir -p "$DEST"
if [ -z "${MTPORT_INSTALL_DIR:-}" ]; then
  # Quit a running copy first so its agent and mount worker are replaced cleanly.
  osascript -e 'tell application "MTPort" to quit' >/dev/null 2>&1 || true
  sleep 2
fi
rm -rf "$DEST/MTPort.app"
cp -R "$APP" "$DEST/"
echo "==> Installed $DEST/MTPort.app"
[ "$OPEN" = 1 ] && open "$DEST/MTPort.app"
echo "Plug in your phone: unlock it, pick USB > File transfer, and allow access to data."
