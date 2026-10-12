# Troubleshooting

[Türkçe](TROUBLESHOOTING.tr.md) · [Back to the README](../README.md)

Most problems come from the phone, the cable, or another app holding the phone, not from MTPort. Work through
[the checklist](#first-the-checklist) first; the sections below are in the order people usually hit them.

**Quick diagnostics** (the commands are in `build/native/cli/mtport`, or in
`~/Applications/MTPort.app/Contents/Resources/bin/mtport`):

```bash
mtport doctor                  # macOS, libmtp, competing apps, USB candidates, free space
mtport devices                 # is the phone on the USB bus?
mtport status                  # active mounts and their state
mount | grep webdav            # is the phone mounted at all?
ls ~/Library/Application\ Support/MTPort/runtime/    # worker logs: mount-<device>.log (no file names inside)
```

## First, the checklist

1. The cable carries **data** (some cables only charge). Try another cable and a port directly on the Mac, not a hub.
2. The phone screen is **on and unlocked**.
3. The USB notification on the phone says **File transfer (MTP)** (not *Charging only*, not *PTP*).
4. If the phone asks to *allow access to data* or to *allow this USB accessory*, you accepted. On Apple Silicon Macs,
   macOS may also ask *Allow accessory to connect?* the first time; allow it (*System Settings → Privacy & Security*).
5. No other app is using the phone: Android File Transfer, OpenMTP, MacDroid, Image Capture, Photos, Smart Switch.
6. **Unplug, wait 5 seconds, plug in again with the screen on.** This fixes more than any other step.

## The phone shows up, but it is empty or says "Operation not permitted"

**Symptom.** A volume appears (or MTPort says *Connecting…* / *Phone detected: select File Transfer*), but folders are
empty, Finder says *Operation not permitted*, or the worker log says
`MtpNotReady: the phone did not allow access to its storage`.

**Cause.** The USB connection is up but the phone has not opened its storage yet: the screen is locked, the *allow access
to data* prompt is waiting, or the USB mode is not File transfer. Some phones, Samsung included, also need 10–60 seconds
after plugging in, and ask again after every re-plug.

**Fix.** Unlock the phone, pick **File transfer (MTP)**, accept the prompt, then unplug and re-plug with the screen on.
MTPort waits up to 90 seconds for the storage and retries automatically (every 10 seconds, up to 6 attempts). If those
are used up, quit and reopen MTPort. MTPort never shows an empty volume for an unreadable phone; if you see one, please
file a bug.

## Nothing appears in Finder

* **Is MTPort running?** The phone is only mounted while the app (and its agent) is running. Open it from the Dock.
* **Is it mounted?** `mount | grep webdav` should show a line ending in `/Volumes/MTPort-<model>`. If yes but the
  sidebar is empty, enable *Finder → Settings → Sidebar → Locations → **Connected servers***. Then try
  `killall Finder`.
* **You ejected it.** After *Eject* the phone is deliberately not mounted again until you unplug and re-plug the cable (or
  press *Mount* in MTPort), so it does not pop back up while you are pulling the plug.
* **The app is waiting for the phone.** See the previous section.
* Still nothing: run `mtport doctor`, `mtport devices` and `mtport status`.

## The sidebar shows `127.0.0.1` instead of the phone's name

Early builds mounted the volume from a plain `127.0.0.1` address, and Finder shows the *host name* of that address.
Current builds publish the phone's own name for the loopback address (through the macOS mDNS service) and use it, so the
sidebar shows e.g. *Galaxy S21 FE*. Update to the latest version. If the name cannot be published (for example it
collides with another device on your network), MTPort falls back to a `<name>.localhost` host name and still mounts.

## "DeviceBusy: could not open an MTP session"

Another program owns the phone: Android File Transfer, OpenMTP, MacDroid, Image Capture, Photos, a second copy of MTPort
or the old `AndroidFinder`. `mtport doctor` lists known competing processes. Quit them (also from the menu bar), then
unplug and re-plug. macOS' own `ptpcamerad` helper can grab a phone that is in PTP/camera mode; choose File transfer
(MTP) instead.

## "libusb_open() failed … Operation timed out", or the phone vanishes from USB

The phone's USB side stopped answering. It can happen after a very heavy transfer, when a cable is marginal, or if a
program that talks to the phone is killed mid-transfer. Unplug and re-plug (try another cable and port). MTPort cannot
fix a wedged USB stack from software.

Tips that avoid it: eject before unplugging; do not run tools that read the volume byte by byte (for example
`dd bs=1`), which makes the phone stream gigabytes; do not run `lsof +D /Volumes/MTPort-…` (it walks the whole phone).

## Big videos open slowly

macOS reads a file from a network volume by downloading it in the background and, in parallel, asking the server for
byte ranges. For a range that runs past the end of the file, macOS' client mishandles the server's (correct) short answer
and would hand your program extra zero bytes, which corrupts copies. MTPort therefore refuses such requests, and
macOS waits for its own download to reach that part of the file instead. Result: the *beginning* of any file, and any read
that stays inside the file, is instant, but the **tail** of a very large file is available only after the download
reached it. Measured on a USB 2.0 phone: 62 MB video ≈ 2.5 s, 1.6 GB video ≈ 50 s (a Quick Look preview or a player
that reads the MP4 index at the end of the file waits that long). Copying is not affected. A faster cable/port
(USB 3 on both sides) helps; nothing else can, because the limit sits in the macOS client.

## The first open of a big folder takes a few seconds

The phone describes files one at a time (about 5 ms each). `DCIM/Camera` with ~1,800 photos takes 4–8 seconds the first
time; MTPort reads `DCIM/Camera`, `Download` and `Pictures` in the background right after mounting and remembers what
it learned, so later visits are instant and only new files cost time. Photos taken or deleted on the phone appear
within a few seconds; a changed *size/date* of an existing file can lag by up to five minutes (opening or copying the file always
asks the phone for the real size).

## "Operation not supported" / "Operation not permitted" when saving, replacing or deleting

These are the safety rules, not bugs ([docs/DATA_SAFETY.md](DATA_SAFETY.md)):

* **Overwrite / save-over** → refused. Copy to a new name, or delete the old file first.
* **In-place edit** → refused. Edit a copy on the Mac, then copy it back under a new name.
* **Move between folders** → refused (MTP cannot do it atomically). Copy, verify, then delete.
* **Delete a non-empty folder in one step** → refused. Delete its contents first.
* **Spotlight, `chmod`, `chown`, extended attributes** → not available on the phone.

If copying a *new* file fails with "Operation not permitted", check that the storage is writable (an SD card can be
locked) and that the phone is unlocked.

## Copies are slightly larger than the original (extra zero bytes)

This was a real bug in early development builds: files whose tail macOS fetched with a range request were rounded up to
the next 1 MiB with zeros. It is fixed in the current version (verified: 123 of 123 cold copies byte-identical to a plain HTTP download of the same files). If you still see it, you are running an old build: reinstall, and compare `shasum -a 256` of a copy with the
original.

## The volume has a `-1` suffix (`/Volumes/MTPort-<model>-1`)

macOS appends `-1` when a stale, empty mount-point folder with the same name is left behind in `/Volumes` (it belongs to
root, so it cannot be removed without admin rights). It is harmless and goes away after a restart.

## I cannot eject it, or a mount is stuck

```bash
mtport unmount --force
diskutil unmount force /Volumes/MTPort-<model>
```

Then quit MTPort and start it again. Make sure nothing (a Terminal tab, `lsof`, a backup program) is working inside the
volume.

## Capacity or free space looks wrong in Finder

The phone is presented as a WebDAV network volume, and Finder's capacity display for such volumes is limited. The
MTPort window shows the capacity the phone reports.

## macOS says the app can't be opened ("unidentified developer")

MTPort is not signed or notarized. An app you build yourself is not quarantined and opens normally. For a copied build:
right-click → *Open*, or `xattr -dr com.apple.quarantine /path/to/MTPort.app`.

## Logs and bug reports

* Worker logs: `~/Library/Application Support/MTPort/runtime/mount-<device>.log`. They contain counters and state, never
  file names or serial numbers, but read them before you paste them.
* HTTP-level trace (file names appear in it, so do not share it blindly):
  `MTPORT_WEBDAV_TRACE=1 mtport mount --read-only`, then read the worker log.
* Please include the output of `mtport doctor` and `mtport status`, your phone model and Android version, macOS version
  and the cable/port. The [bug report template](../.github/ISSUE_TEMPLATE/bug_report.md) lists everything we need.

## Only if you use the experimental `--transport fskit` (macFUSE)

* macFUSE volumes are **not listed in the Finder sidebar** (a known macFUSE limitation); open them from `/Volumes`.
* Install macFUSE ≥ 5.4.0 and enable its file-system extension (*System Settings → General → Login Items & Extensions →
  File System Extensions*). Older versions can corrupt small writes.
* Never add macFUSE's `local` mount option: it produced mounts that could not be unmounted until a reboot.
* A mount stuck after a crash: `diskutil unmount force /Volumes/<name>`, and if that fails, restart the Mac.
