#!/usr/bin/env bash
# Everything that can run WITHOUT a phone and WITHOUT macFUSE: unit tests, fake-filesystem
# integration tests, CLI against the mock backend. Prints a truthful summary of what was NOT run.
set -uo pipefail
cd "$(dirname "$0")/.."
export PATH="$(brew --prefix 2>/dev/null || echo /opt/homebrew)/bin:/usr/local/bin:$PATH"
BUILD="${1:-build}"
[ -d "$BUILD" ] || { echo "no $BUILD directory: run scripts/build-native.sh first" >&2; exit 1; }
cmake --build "$BUILD" || exit 1
ctest --test-dir "$BUILD" --output-on-failure
rc=$?
echo
echo "NOT RUN by this script (need real hardware / macFUSE): real USB MTP, Finder mount, FSKit EOF regression on a live mount."
echo "See docs/HARDWARE_ACCEPTANCE.md and tests/scripts/verify_mount.sh."
exit $rc
