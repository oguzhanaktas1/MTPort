#!/usr/bin/env bash
# Product code must never call device-destroying libmtp functions.
set -u
ROOT="${1:?usage: $0 <repo root>}"
status=0
for sym in LIBMTP_Format_Storage LIBMTP_Reset_Device; do
  if grep -rn --include='*.cpp' --include='*.hpp' --include='*.h' --include='*.swift' "$sym" "$ROOT/native" "$ROOT/macos" 2>/dev/null; then
    echo "FORBIDDEN API USED: $sym"
    status=1
  fi
done
[ $status -eq 0 ] && echo "forbidden API check: clean (no LIBMTP_Format_Storage / LIBMTP_Reset_Device)"
exit $status
