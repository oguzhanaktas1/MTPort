# Changelog

## 0.1.0 (developer preview)

First public version. Tested on one phone (Samsung Galaxy S21 FE, macOS 26, Apple Silicon); see
[docs/DEVICE_COMPATIBILITY.md](docs/DEVICE_COMPATIBILITY.md).

### Added

* **Finder integration without macFUSE.** The phone is served to macOS by a loopback-only WebDAV server (random port,
  random per-mount password) and mounted with macOS' own network-volume support. It appears in the Finder sidebar with an
  eject button and needs no kernel extension, no macFUSE and no admin rights. The sidebar entry carries the phone's own
  name (published for `127.0.0.1` through the system mDNS service; spaces become no-break spaces because the macOS client
  cannot mount a host name with a plain space; falls back to `<name>.localhost`).
* **Auto-mount and open in Finder** when a phone is plugged in; re-mount after unplug/replug; an *Eject* is respected until
  the cable is really unplugged. The worker refuses to show an unreadable (locked/unauthorised) phone as an empty volume and
  waits up to 90 s for the storage instead.
* **Safe writes.** Files copied to the phone are staged on the Mac and uploaded when closed; failed uploads keep their
  bytes. No overwrite, no in-place edit, no recursive delete, no cross-folder move. Finder's `.DS_Store` / `._*` files stay
  in memory.
* **Fast listings.** The phone is asked for each object with `GetObjectInfo` (about 4x faster than libmtp's default
  `GetObjPropList` on a Galaxy, identical results), object metadata is remembered for the session, `DCIM/Camera`, `Download`
  and `Pictures` are read in the background after mounting, and long listings step aside for other requests and are
  cancelled at eject. First open of a ~1,800-photo folder: 4–8 s instead of 35–48 s; repeat visits < 1 s.
* **Streaming.** 16 MiB `GetPartialObject` requests (about 1.9x faster than 1 MiB ones: ~17.5 → ~33 MB/s on USB 2.0 High
  Speed); reads and downloads always ask the phone for the real file size first.
* C++20 core (`libmtp`/`libusb`, RAII, typed errors, one MTP session owner per phone), `mtport` command line
  (`doctor`, `devices`, `info`, `ls`, `pull`, `push`, `mkdir`, `rm`, `mv`, `mount`, `unmount`, `status`, `bench`),
  `mtport-agent` (versioned JSON control socket) and a SwiftUI app (menu bar + window, Turkish/English).
* An experimental macFUSE/FSKit transport (`mtport mount --transport fskit`).
* Tests: 141 C++ unit/integration tests against fake phones (also under ASan/UBSan), Swift tests (incl. a live
  Swift ↔ C++ agent contract), mock-volume and real-phone verification scripts.

### Fixed during development (good to know when comparing with older builds)

* Copies from the volume could end with extra zero bytes (up to 1 MiB): the macOS WebDAV client zero-pads a server's
  correct short answer to a `Range` request that runs past the end of the file. Such ranges are now refused with `416`.
  123 of 123 cold copies are byte-identical to a plain download (before: 12 of 40 photos were padded). Trade-off: the tail of a
  very large file (for example 1.6 GB: ~50 s) is readable only once the client's own download reaches it.
* A mount that stalled for ~90 s (quota properties during mount), a hang with `MNT_RDONLY`, silent false success of refused
  write-backs, a crash when the phone was unplugged, and an automatic re-mount right after an eject.

### Known limitations

* Only one phone model has been tested; sleep/wake with a mounted phone and unplug during a transfer were not exercised on
  hardware.
* Not signed or notarized: build it yourself (`scripts/install-app.sh`).
* The very first listing of a big folder that was not pre-read is still bound by the phone (~5 ms per file).

### Naming

The project was called *AndroidFinder* while it was being developed. On first start of MTPort, settings and the upload
staging area are moved from `~/Library/Application Support/AndroidFinder` and `~/Library/Caches/AndroidFinder` to
`.../MTPort`.
