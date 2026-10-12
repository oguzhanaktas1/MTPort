# Testing on a real phone

A checklist for trying MTPort on a phone and reporting the result. Send the filled-in table (and your phone model) as a
new row of [DEVICE_COMPATIBILITY.md](DEVICE_COMPATIBILITY.md) via pull request or issue. Use only test data; never write
into DCIM/Pictures.

## Preconditions
* macOS 15.4+, `scripts/build-native.sh --test` green, `mtport doctor` without failures. macFUSE is **not** needed (the default WebDAV transport).
* Phone unlocked, USB → File transfer (MTP), data-capable cable, no competing MTP app.

## Steps

```bash
# 1. USB visible (does not prove MTP)
system_profiler SPUSBDataType
# 2. libmtp sees it (no other owner!)
mtp-detect
# 3. MTPort sees it; no fabricated storages
mtport devices --json
mtport info
# 4. lazy listing of the real tree
mtport ls --storage <id> --path /
mtport ls --storage <id> --path /DCIM
# 5. pull a 1-10 MB file you also have on the Mac, compare hashes
mtport pull --storage <id> --src /DCIM/Camera/<photo.jpg> --dst ~/Desktop/photo.jpg
shasum -a 256 ~/Desktop/photo.jpg <your original copy>
# 6. read-only Finder mount
mtport mount --foreground        # (or without --foreground, then:)
mtport status
open "/Volumes/MTPort-<name>"    # Quick Look a photo (Space); drag a file to the Desktop; compare sha256
tests/scripts/verify_mount.sh --volume "/Volumes/MTPort-<name>" --read-only
# 7. writes (P1) - only new test data:
mtport unmount && mtport mount --read-write
mtport push --storage <id> --src ~/Desktop/test.bin --dst /Download/MTPortTest/test.bin --verify   # (mkdir first)
tests/scripts/verify_mount.sh --volume "/Volumes/MTPort-<name>" --write
# 8. unplug during a transfer (large file); expect: ENODEV/errors, no hang, staged bytes kept, volume unmounted, remount works
# 9. sleep/wake with the volume mounted:  pmset sleepnow ; wake ;  (or:  kill -USR1 <mtport-mount pid>)
# 10. a folder with >= 1000 photos: record count + time only (no names)
time ls -f "/Volumes/MTPort-<name>/Internal Storage/DCIM/Camera" | wc -l
```

Mock rehearsal of 6-9 (no phone needed): `tests/scripts/verify_mount.sh --mock [--write]`.

## Result template

| # | Step | Result (PASS/FAIL/BLOCKED/NOT RUN) | Notes (device model, Android version, cable, port, USB speed) |
|---|---|---|---|
| 1-10 | | _fill in_ | |
