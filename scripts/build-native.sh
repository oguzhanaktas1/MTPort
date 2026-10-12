#!/usr/bin/env bash
# Configure + build the native code (and run the test suite with --test).
# usage: scripts/build-native.sh [--release] [--asan] [--test] [--fuse-headers DIR]
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="$(brew --prefix 2>/dev/null || echo /opt/homebrew)/bin:/usr/local/bin:$PATH"

PRESET=debug; RUN_TESTS=0; EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --release) PRESET=release ;;
    --asan) PRESET=asan ;;
    --test) RUN_TESTS=1 ;;
    --fuse-headers) shift; EXTRA+=("-DMTPORT_FUSE_INCLUDE_DIR=$1") ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
for t in cmake ninja; do command -v "$t" >/dev/null || { echo "missing $t - run scripts/bootstrap.sh" >&2; exit 1; }; done

cmake --preset "$PRESET" "${EXTRA[@]+"${EXTRA[@]}"}"
cmake --build --preset "$PRESET"
if [ "$RUN_TESTS" = 1 ]; then
  ctest --preset "$PRESET" 2>/dev/null || ctest --test-dir "$(cmake -L -N --preset "$PRESET" >/dev/null 2>&1; case $PRESET in release) echo build-release;; asan) echo build-asan;; *) echo build;; esac)" --output-on-failure
fi
