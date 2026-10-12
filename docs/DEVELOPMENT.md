# Development

## Build and test

```bash
brew install libmtp libusb cmake ninja pkgconf xcodegen
scripts/bootstrap.sh                    # read-only check: prints what is missing, installs nothing
scripts/build-native.sh --test          # debug build + all tests (a fake phone, no hardware needed)
scripts/build-native.sh --asan --test   # AddressSanitizer + UBSan
tests/scripts/check_forbidden_api.sh .  # product code must never call LIBMTP_Format_Storage / LIBMTP_Reset_Device
```

By hand: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build && ctest --test-dir build --output-on-failure`.
The test binary is `build/tests/mtport-tests` (it also takes a suite name: `mtport-tests webdav`).

CMake options:

| Option | Default | Meaning |
|---|---|---|
| `MTPORT_WITH_LIBMTP` | ON | real MTP backend (needs libmtp via pkg-config) |
| `MTPORT_WITH_MOCK` | ON | fake phones used by tests and `--backend fake` |
| `MTPORT_WITH_FUSE` | ON | build the experimental macFUSE transport when macFUSE is found; OFF skips it |
| `MTPORT_SANITIZE` | OFF | ASan + UBSan |
| `MTPORT_FUSE_INCLUDE_DIR` | – | directory with libfuse 2.9 headers, to *compile-check* the FUSE adapter without macFUSE |

## The app

```bash
scripts/install-app.sh                  # Release build of everything, installs ~/Applications/MTPort.app
# or step by step:
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
cd macos/MTPort && xcodegen generate
xcodebuild -project MTPort.xcodeproj -scheme MTPort -configuration Debug build
xcodebuild -project MTPort.xcodeproj -scheme MTPort test      # Swift tests incl. a live Swift <-> C++ agent contract
```

`MTPort.xcodeproj` is generated from `macos/MTPort/project.yml` and not committed. A build phase copies `mtport`,
`mtport-agent` and `mtport-mount` from `build/native` into `MTPort.app/Contents/Resources/bin`. Note that
`xcodebuild test` launches the host app, which auto-mounts a connected phone on your real home directory; quit it
afterwards. The bundled tools link Homebrew's libmtp dylib, so a self-contained, signed, notarized build is future work.

## Trying it without a phone

```bash
mkdir -p /tmp/fakephone && echo hi > /tmp/fakephone/a.txt
build/native/cli/mtport mount --backend fake --fake-root /tmp/fakephone --read-only    # appears as /Volumes/MTPort-Mock-Phone-…
tests/scripts/verify_mount.sh --mock                    # read/EOF/short-read checks on a mock volume (12 checks)
tests/scripts/verify_mount.sh --mock --write            # + write checks
build/native/cli/mtport unmount
```

With a real phone that is already mounted, `tests/scripts/verify_mount.sh --volume /Volumes/MTPort-<model> --write`
runs the write checks (creates and removes only `Download/MTPortMountTest/`). See
[HARDWARE_ACCEPTANCE.md](HARDWARE_ACCEPTANCE.md).

## Environment variables

| Variable | Use |
|---|---|
| `MTPORT_HOME` | relocate all per-user state (tests use it) |
| `MTPORT_AGENT_PATH` | tell the app where `mtport-agent` is |
| `MTPORT_WEBDAV_TRACE=1` | log every HTTP request/response size in the worker log (contains file names) |
| `MTPORT_WEBDAV_SERVE_ONLY=1` | worker prints port/user/password of its WebDAV server and does not mount (for `curl`) |
| `MTPORT_WEBDAV_RANGE_MODE=clamp` | answer over-long `Range` requests the RFC way (see below); for experiments only |
| `MTPORT_SIDEBAR_NAME` | override the name published for the sidebar |
| `MTPORT_FUSE_DEBUG`, `MTPORT_FUSE_OPTS` | extra libfuse debugging/options for the fskit transport |

## Files on disk

| What | Where |
|---|---|
| agent socket (0600), mount state, device locks, worker logs | `~/Library/Application Support/MTPort/runtime/` (0700) |
| write staging (0700 dir, 0600 files) | `~/Library/Caches/MTPort/Staging/` |
| read fallback cache (disposable) | `~/Library/Caches/MTPort/ReadCache/` |

## Design notes worth knowing before you change things

* **One owner per phone.** `LibmtpDevice` serialises every call behind a mutex; a cross-process `flock` stops a second
  worker/CLI from opening the same phone (`DeviceBusy`). Long listings release the lock between objects so that reads
  are not starved, and can be cancelled at eject.
* **Listing speed.** libmtp describes each object with one MTP request. The session is opened with libmtp's
  `BROKEN_MTPGETOBJPROPLIST` flag so it uses `GetObjectInfo` (about 4x faster on a Galaxy, identical results), the object
  metadata is remembered for the session (handle list re-read every time, objects younger than 5 minutes reused), and the
  worker pre-reads `DCIM/Camera`, `Download`, `Pictures` after mounting. Reads and `GET` ask the phone for the real size.
* **Chunk size.** The phone has a fixed cost per request; 16 MiB `GetPartialObject` requests stream about twice as fast as
  1 MiB ones. Small reads still request only what was asked.
* **macOS WebDAV client quirks, all handled in `native/webdav`:**
  * Quota properties during the initial mount make `NetFSMountURLSync` wait ~90 s: they are only served after mounting.
  * Creating a file is an empty `PUT`, then `LOCK`, then a second `PUT` with the content: the empty one only creates a
    placeholder that is uploaded when content arrives.
  * A refused write-back is swallowed by the client (it reports success), so `LOCK` on an existing phone file is refused
    instead of the write.
  * `MNT_RDONLY` makes `NetFSMountURLSync` hang: read-only is expressed by not advertising WebDAV class 2.
  * Spotlight is told not to index the volume with a virtual `/.metadata_never_index`.
  * A `Range` that runs past EOF is answered `416`: the client zero-pads a short `206` to the requested length and
    returns that padding to the reader (`webdav_read_bytes` copies the *requested* count), which corrupted copies.
    This is why the tail of huge files waits for the client's download.
  * The sidebar shows the host name of the mount URL: the worker publishes `<phone name>` for `127.0.0.1` through
    `DNSServiceRegisterRecord` (spaces become U+00A0; plain spaces make the mount fail) and falls back to `.localhost`.
* **Eject semantics.** After an eject, a marker keyed by USB vendor:product keeps auto-mount away until the phone has
  really been unplugged (the phone re-enumerates under a new bus address right after an eject).
* **Readiness gate.** The worker refuses to mount a phone whose storage queries fail (locked/unauthorised) instead of
  showing an empty volume; it retries for up to 90 s.

## The experimental macFUSE (FSKit) transport

`mtport mount --transport fskit` needs macFUSE ≥ 5.4.0 with its FSKit extension enabled (*System Settings → General →
Login Items & Extensions → File System Extensions*) and is built when macFUSE's `libfuse` is found. Known limits: the
volume is not listed in the Finder sidebar (macFUSE issue #1114), do not add the `local` option (un-unmountable mounts),
and macFUSE 5.4.0 has a reported zero-length-read problem (#1196) that `verify_mount.sh --transport fskit` exercises.
