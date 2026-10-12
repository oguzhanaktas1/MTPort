#!/usr/bin/env bash
# Finder integration check (WebDAV network volume or macFUSE/FSKit) for a MOCK volume or a real phone volume that is ALREADY MOUNTED.
# The default WebDAV transport needs no macFUSE; with --transport fskit it needs macFUSE >= 5.4.0 + the FSKit extension.
# It is NOT part of ctest (it mounts a volume on the host) and reports NOT RUN when prerequisites are missing - never PASS.
#
#   mock volume:   tests/scripts/verify_mount.sh --mock
#   real phone:    tests/scripts/verify_mount.sh --volume "/Volumes/MTPort-<name>" --read-only
#                  (add --write to also run the write checks; they only create files inside
#                   MTPortTest/ on the volume and remove exactly those afterwards)
set -uo pipefail
cd "$(dirname "$0")/../.."
BIN=build/native
MOCK=0; WRITE=0; VOLUME=""; TRANSPORT=webdav
while [ $# -gt 0 ]; do case "$1" in
  --mock) MOCK=1 ;; --write) WRITE=1 ;; --read-only) WRITE=0 ;;
  --volume) shift; VOLUME="$1" ;;
  --transport) shift; TRANSPORT="$1" ;;
  *) echo "unknown option $1"; exit 2 ;; esac; shift; done

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS    $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL    $1"; }
skip() { SKIP=$((SKIP+1)); echo "  NOT RUN $1"; }

# --- prerequisites: refuse to fake a result -------------------------------------------------
if [ "$TRANSPORT" = fskit ]; then
  V="$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null || true)"
  if [ -z "$V" ]; then echo "NOT RUN: macFUSE is not installed (see docs/SETUP_MACOS.md)."; exit 3; fi
  echo "macFUSE $V (FSKit transport)"
else
  echo "WebDAV transport (macOS network volume, no macFUSE needed)"
fi
[ -x "$BIN/mount/mtport-mount" ] || { echo "NOT RUN: $BIN/mount/mtport-mount was not built."; exit 3; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/af-mount.XXXXXX")"
cleanup() {
  [ -n "${MOUNTED_BY_US:-}" ] && "$BIN/cli/mtport" unmount --device fake:0002@1.2 --force >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT

if [ "$MOCK" = 1 ]; then
  ROOT="$WORK/phone"; mkdir -p "$ROOT/DCIM" "$ROOT/Download"
  head -c 5000000 /dev/urandom > "$ROOT/DCIM/test.jpg"; : > "$ROOT/DCIM/empty.bin"
  printf '1234567890abcde' > "$ROOT/Download/fifteen.txt"; printf 'x' > "$ROOT/Download/one.txt"
  export MTPORT_HOME="$WORK/home"
  MODE=--read-only; [ "$WRITE" = 1 ] && MODE=--read-write
  "$BIN/cli/mtport" mount --backend fake --fake-root "$ROOT" --transport "$TRANSPORT" $MODE || { echo "mount failed"; exit 1; }
  MOUNTED_BY_US=1
  VOLUME="/Volumes/MTPort-Mock-Phone-host-directory"
  VOLUME="$("$BIN/cli/mtport" status --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["mounts"][0]["mountPoint"])')"
  STORE="$VOLUME/Internal Storage"
  SRC_DCIM="$ROOT/DCIM"; SRC_DL="$ROOT/Download"
else
  [ -n "$VOLUME" ] || { echo "pass --mock or --volume /Volumes/..."; exit 2; }
  STORE="$VOLUME/$(ls "$VOLUME" | head -1)"
  SRC_DCIM=""; SRC_DL=""
fi
case "$VOLUME" in /Volumes/*) ;; *) echo "volume must be under /Volumes"; exit 2 ;; esac
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
# BSD wc pads with spaces ("       0"): normalise, and show the raw value on mismatch.
bytes_of() { wc -c | tr -d '[:space:]'; }
expect_bytes() { # expect_bytes NAME WANT  (reads the byte count from stdin)
  local got; got="$(bytes_of)"
  if [ "$got" = "$2" ]; then ok "$1"; else bad "$1 (wanted $2 bytes, got '$got')"; fi
}

echo "== volume basics"
mount | grep -q " on ${VOLUME} " && ok "volume is mounted at $VOLUME" || bad "volume not in mount table"
mount | grep " on ${VOLUME} " | grep -qi "webdav\|fskit\|macfuse\|fuse" && ok "mount type is a WebDAV network volume or macFUSE" || skip "could not confirm mount type from 'mount' output"
ls -la "$VOLUME" >/dev/null 2>&1 && ok "ls -la" || bad "ls -la"

if [ "$MOCK" = 1 ]; then
  echo "== reads (mock fixtures with known hashes)"
  stat "$STORE/DCIM/test.jpg" >/dev/null 2>&1 && ok "stat" || bad "stat"
  [ "$(sha "$STORE/DCIM/test.jpg")" = "$(sha "$SRC_DCIM/test.jpg")" ] && ok "shasum of 5 MB file equals source" || bad "shasum differs"
  cp "$STORE/DCIM/test.jpg" "$WORK/copy.jpg" && [ "$(sha "$WORK/copy.jpg")" = "$(sha "$SRC_DCIM/test.jpg")" ] && ok "cp to Mac is byte-identical" || bad "cp differs"

  echo "== EOF / zero-length regression (macFUSE issue #1196 class)"
  expect_bytes "0-byte file reads as 0 bytes" 0 < "$STORE/DCIM/empty.bin"
  [ -z "$(cat "$STORE/DCIM/empty.bin")" ] && ok "cat of empty file prints nothing" || bad "cat of empty file printed data"
  [ "$(cat "$STORE/Download/fifteen.txt")" = "1234567890abcde" ] && ok "15-byte file content" || bad "15-byte file content"
  [ "$(cat "$STORE/Download/one.txt")" = "x" ] && ok "1-byte file content" || bad "1-byte file content"
  dd if="$STORE/Download/fifteen.txt" bs=1 skip=15 2>/dev/null | expect_bytes "read at exactly EOF returns 0 bytes" 0
  dd if="$STORE/Download/fifteen.txt" bs=4096 skip=5 2>/dev/null | expect_bytes "read past EOF returns 0 bytes" 0
  dd if="$STORE/Download/fifteen.txt" bs=7 2>/dev/null | expect_bytes "short final block (7+7+1)" 15
  python3 - "$STORE/Download/fifteen.txt" <<'PY' && ok "pread patterns (offsets 0,14,15,100; oversize length)" || bad "pread patterns"
import os, sys
fd = os.open(sys.argv[1], os.O_RDONLY)
assert os.pread(fd, 100, 0) == b"1234567890abcde"
assert os.pread(fd, 5, 10) == b"abcde"
assert os.pread(fd, 1, 14) == b"e"
assert os.pread(fd, 10, 15) == b""
assert os.pread(fd, 10, 100) == b""
os.close(fd)
PY
else
  echo "== reads (real phone: no fixture hashes available - compare against a file you know)"
  skip "hash comparison needs a known original; use: shasum -a 256 <file on volume> and compare with the source"
  ls "$STORE" | head -5 >/dev/null && ok "storage lists" || bad "storage list"
fi

echo "== read-only enforcement"
if touch "$STORE/af-should-fail" 2>/dev/null; then
  if [ "$WRITE" = 1 ]; then rm -f "$STORE/af-should-fail"; else bad "write succeeded on a read-only mount"; fi
else
  [ "$WRITE" = 1 ] && bad "write refused on a read-write mount" || ok "write refused on read-only mount"
fi

if [ "$WRITE" = 1 ]; then
  echo "== writes (inside Download/MTPortMountTest/ only)"
  T="$STORE/Download/MTPortMountTest"
  mkdir "$T" && ok "mkdir" || bad "mkdir"
  for n in 1 2 7 14 15 16 4097 1048576; do
    head -c "$n" /dev/urandom > "$WORK/w$n"
    cp "$WORK/w$n" "$T/w$n" 2>/dev/null
    [ "$(sha "$T/w$n")" = "$(sha "$WORK/w$n")" ] && ok "copy $n bytes: round-trip sha256" || bad "copy $n bytes: round-trip differs"
  done
  printf '' > "$WORK/w0"; cp "$WORK/w0" "$T/w0" && [ ! -s "$T/w0" ] && [ -e "$T/w0" ] && ok "empty file" || bad "empty file"
  printf 'abc' > "$T/echoed" && [ "$(cat "$T/echoed")" = abc ] && ok "shell redirect create" || bad "shell redirect create"
  if echo more >> "$T/echoed" 2>/dev/null; then bad "in-place append unexpectedly succeeded"; else ok "in-place modification refused (EOPNOTSUPP), original intact"; fi
  [ "$(cat "$T/echoed")" = abc ] && ok "original content untouched after refused edit" || bad "original content changed"
  if cp "$WORK/w1" "$T/w15" 2>/dev/null; then bad "overwrite of existing file succeeded"; else ok "overwrite refused"; fi
  [ "$(sha "$T/w15")" = "$(sha "$WORK/w15")" ] && ok "refused overwrite left the original intact" || bad "original damaged"
  mv "$T/w1" "$T/w1-renamed" 2>/dev/null && ok "rename (same folder)" || skip "rename not supported by this device"
  rmdir "$T" 2>/dev/null && bad "rmdir removed a non-empty folder" || ok "rmdir of non-empty folder refused"
  # cleanup: exactly the files this script created
  for f in "$T"/*; do rm -f "$f"; done; rmdir "$T" 2>/dev/null && ok "cleanup removed only test data" || bad "cleanup left items"
fi

echo
echo "verify_mount: $PASS passed, $FAIL failed, $SKIP not run"
[ "$FAIL" -eq 0 ]
