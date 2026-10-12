#!/usr/bin/env bash
# End-to-end CLI check against the MOCK backend (a host directory posing as a phone).
# This proves the CLI/core logic. It does NOT prove anything about real USB, libmtp-on-hardware or Finder.
#
# usage: verify_roundtrip.sh /path/to/mtport
set -uo pipefail

CLI="${1:?usage: $0 /path/to/mtport}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/af-roundtrip.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
export MTPORT_HOME="$WORK/home"
ROOT="$WORK/phone"
LOCAL="$WORK/local"
mkdir -p "$ROOT/DCIM/Camera" "$ROOT/Download" "$LOCAL"
FAKE=(--backend fake --fake-root "$ROOT")
PASS=0; FAILS=0

ok()   { PASS=$((PASS+1)); echo "  ok   - $1"; }
bad()  { FAILS=$((FAILS+1)); echo "  FAIL - $1"; }
check() { local name="$1"; shift; if "$@"; then ok "$name"; else bad "$name"; fi; }
sha()  { shasum -a 256 "$1" | cut -d' ' -f1; }
expect_exit() { # expect_exit CODE NAME cmd...
  local want="$1" name="$2"; shift 2
  "$@" >/dev/null 2>&1; local got=$?
  if [ "$got" -eq "$want" ]; then ok "$name (exit $got)"; else bad "$name: wanted exit $want, got $got"; fi
}

# fixtures
head -c 4000000 /dev/urandom > "$ROOT/DCIM/Camera/photo.jpg"
printf 'hello' > "$ROOT/Download/hi.txt"
: > "$ROOT/Download/empty.bin"
printf 'turkce' > "$ROOT/Download/"$'T\xc3\xbcrk\xc3\xa7e \xc5\x9f\xc4\xb1k.txt'
SRC_SHA="$(sha "$ROOT/DCIM/Camera/photo.jpg")"

echo "== devices / info"
OUT="$("$CLI" devices --json "${FAKE[@]}")"
check "devices lists exactly the mock phone" test "$(echo "$OUT" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')" = 1
check "devices marks mtpReady as not probed (null)" test "$(echo "$OUT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["devices"][0]["mtpReady"])')" = None
check "info reports storages" bash -c "'$CLI' info --json ${FAKE[*]@Q} | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d[\"storages\"][0][\"name\"]==\"Internal Storage\"'"

echo "== ls is non-recursive and exact"
check "ls root" bash -c "'$CLI' ls ${FAKE[*]@Q} --path / | grep -q 'DCIM/'"
check "ls /DCIM/Camera" bash -c "'$CLI' ls ${FAKE[*]@Q} --path /DCIM/Camera | grep -q photo.jpg"
expect_exit 6 "ls missing path" "$CLI" ls "${FAKE[@]}" --path /nope
expect_exit 2 "ls traversal path rejected" "$CLI" ls "${FAKE[@]}" --path /DCIM/../..

echo "== pull"
"$CLI" pull "${FAKE[@]}" --src /DCIM/Camera/photo.jpg --dst "$LOCAL/photo.jpg" >/dev/null
check "pull sha256 equals source" test "$(sha "$LOCAL/photo.jpg")" = "$SRC_SHA"
expect_exit 7 "pull refuses to overwrite existing local file" "$CLI" pull "${FAKE[@]}" --src /DCIM/Camera/photo.jpg --dst "$LOCAL/photo.jpg"
check "existing local file untouched" test "$(sha "$LOCAL/photo.jpg")" = "$SRC_SHA"
"$CLI" pull "${FAKE[@]}" --src /Download/empty.bin --dst "$LOCAL/empty.bin" >/dev/null
check "empty file pulls as empty" test ! -s "$LOCAL/empty.bin" -a -e "$LOCAL/empty.bin"
mkdir "$LOCAL/dir"
"$CLI" pull "${FAKE[@]}" --src /Download/hi.txt --dst "$LOCAL/dir" >/dev/null
check "pull into a directory keeps the name" test "$(cat "$LOCAL/dir/hi.txt")" = hello
"$CLI" pull "${FAKE[@]}" --src "/Download/Türkçe şık.txt" --dst "$LOCAL/tr.txt" >/dev/null
check "non-ASCII name pulls" test "$(cat "$LOCAL/tr.txt")" = turkce
expect_exit 1 "pull of a folder is refused" "$CLI" pull "${FAKE[@]}" --src /DCIM --dst "$LOCAL/x"
check "no temp debris left next to downloads" test -z "$(ls -A "$LOCAL" | grep afpart || true)"

echo "== push (new files only)"
for n in 0 1 14 15 1048576; do
  if [ "$n" -eq 0 ]; then : > "$LOCAL/push$n.bin"; else head -c "$n" /dev/urandom > "$LOCAL/push$n.bin"; fi
  "$CLI" push "${FAKE[@]}" --src "$LOCAL/push$n.bin" --dst "/Download/push$n.bin" --verify >/dev/null
  check "push $n bytes: round trip sha256" test "$(sha "$ROOT/Download/push$n.bin")" = "$(sha "$LOCAL/push$n.bin")"
done
expect_exit 7 "push refuses to overwrite" "$CLI" push "${FAKE[@]}" --src "$LOCAL/push1.bin" --dst /Download/push1.bin
check "original of refused overwrite intact" test "$(sha "$ROOT/Download/push1.bin")" = "$(sha "$LOCAL/push1.bin")"
expect_exit 6 "push into missing folder" "$CLI" push "${FAKE[@]}" --src "$LOCAL/push1.bin" --dst /Missing/x.bin
expect_exit 2 "push of a non-file" "$CLI" push "${FAKE[@]}" --src "$LOCAL" --dst /Download/x

echo "== mkdir / rm / mv"
"$CLI" mkdir "${FAKE[@]}" --path /Download/MTPortTest --parents >/dev/null
check "mkdir created a real folder" test -d "$ROOT/Download/MTPortTest"
expect_exit 7 "mkdir existing" "$CLI" mkdir "${FAKE[@]}" --path /Download/MTPortTest
"$CLI" push "${FAKE[@]}" --src "$LOCAL/push14.bin" --dst /Download/MTPortTest/f.bin >/dev/null
expect_exit 1 "rm folder needs --dir" "$CLI" rm "${FAKE[@]}" --path /Download/MTPortTest
expect_exit 1 "rm --dir on a non-empty folder is refused" "$CLI" rm "${FAKE[@]}" --path /Download/MTPortTest --dir
check "non-empty folder and its file survive" test -f "$ROOT/Download/MTPortTest/f.bin"
"$CLI" mv "${FAKE[@]}" --path /Download/MTPortTest/f.bin --new-name g.bin >/dev/null
check "mv renamed" test -f "$ROOT/Download/MTPortTest/g.bin" -a ! -e "$ROOT/Download/MTPortTest/f.bin"
expect_exit 7 "mv onto an existing name is refused" "$CLI" mv "${FAKE[@]}" --path /Download/push1.bin --new-name push0.bin
"$CLI" rm "${FAKE[@]}" --path /Download/MTPortTest/g.bin >/dev/null
"$CLI" rm "${FAKE[@]}" --path /Download/MTPortTest --dir >/dev/null
check "rm removed exactly the targets" test ! -e "$ROOT/Download/MTPortTest" -a -f "$ROOT/Download/hi.txt"

echo "== single MTP owner"
LOCKDIR="$MTPORT_HOME/Support/runtime/locks"
python3 - "$LOCKDIR/fake-0002-1.2.lock" <<'PY' &
import fcntl, sys, time
f = open(sys.argv[1], "a+")
fcntl.flock(f, fcntl.LOCK_EX)
open(sys.argv[1] + ".held", "w").close()
time.sleep(30)
PY
HOLDER=$!
for _ in $(seq 1 50); do [ -e "$LOCKDIR/fake-0002-1.2.lock.held" ] && break; sleep 0.1; done
expect_exit 3 "second owner gets DeviceBusy (exit 3)" "$CLI" info "${FAKE[@]}"
check "devices reports the foreign owner" bash -c "'$CLI' devices --json ${FAKE[*]@Q} | grep -q '\"ownedByAnotherProcess\":true'"
kill "$HOLDER" 2>/dev/null; wait "$HOLDER" 2>/dev/null
sleep 0.2
expect_exit 0 "device usable again after the owner exits" "$CLI" info "${FAKE[@]}"

echo "== json errors / usage"
check "json error shape" bash -c "'$CLI' ls ${FAKE[*]@Q} --path /nope --json | python3 -c 'import json,sys; assert json.load(sys.stdin)[\"error\"][\"code\"]==\"ObjectNotFound\"'"
expect_exit 2 "unknown option" "$CLI" devices --bogus
expect_exit 2 "unknown command" "$CLI" frobnicate
expect_exit 2 "no command" "$CLI"
expect_exit 0 "--version" "$CLI" --version

echo "== bench never mutates the phone"
BEFORE="$(find "$ROOT" -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256)"
"$CLI" bench "${FAKE[@]}" --direction pull --file /DCIM/Camera/photo.jpg --runs 2 --json >/dev/null
"$CLI" bench "${FAKE[@]}" --direction pull --file /DCIM/Camera/photo.jpg --mode partial --chunk-kib 256 --json >/dev/null
AFTER="$(find "$ROOT" -type f -print0 | sort -z | xargs -0 shasum -a 256 | shasum -a 256)"
check "phone content unchanged by bench" test "$BEFORE" = "$AFTER"
expect_exit 2 "bench rejects push direction" "$CLI" bench "${FAKE[@]}" --direction push --file /x

echo
echo "verify_roundtrip: $PASS passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
