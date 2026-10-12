<p align="center"><img src="docs/assets/icon.png" width="128" alt="MTPort icon"></p>

# MTPort

**Your Android phone as a normal drive in Finder.** Plug in the cable, and the phone shows up in the Finder sidebar
under its own name, with an eject button. No Android File Transfer window, no ADB, no phone-side app, no cloud, no
root, no kernel extension.

[Türkçe README](README.tr.md) · [Troubleshooting](docs/TROUBLESHOOTING.md) · [Data safety](docs/DATA_SAFETY.md) ·
[Changelog](CHANGELOG.md)

> **Status: developer preview (v0.1.0).** It is not signed or notarized, so you build it yourself (one command,
> below). Please read [What is (not) supported](#what-is-not-supported) before you trust it with files you cannot lose.

## Screenshots

<table>
<tr>
<td align="center" valign="top" width="34%"><img src="docs/assets/screenshot-app.png" alt="The MTPort window: the phone is mounted, with capacity, Open in Finder and Eject"><br><sub>The MTPort window: state, capacity, <i>Open in Finder</i> / <i>Eject</i>, transfers.</sub></td>
<td align="center" valign="top" width="66%"><img src="docs/assets/screenshot-finder.png" alt="Finder with the phone in the sidebar under Locations and its storage open"><br><sub>The phone in the Finder sidebar (under <i>Locations</i>, with an eject button) and its storage open in a normal Finder window.</sub></td>
</tr>
</table>

## What it does

* Detects a phone in **USB "File transfer (MTP)"** mode, mounts it, and opens it in Finder. It re-mounts after you
  unplug and plug the cable again.
* Browse folders, copy photos/videos/files **from** the phone, and copy new files **to** it, in Finder or the Terminal.
* Menu-bar item and a small window with the phone's state, capacity, transfers, *Mount*, *Open in Finder*, *Eject*.
* Large folders (about 1,800 photos) open in a few seconds, and repeat visits are instant.
* Everything stays on your Mac: no network access beyond a loopback connection to macOS itself (see
  [How it works](#how-it-works)).

## What is (not) supported

MTP is an *object* protocol, not a disk, so a few things work differently from a USB stick. These limits protect your
data and are by design:

| Works | Does **not** work (fails with a clear error) |
|---|---|
| Browse, preview (Quick Look), copy files off the phone | Overwriting a file that already exists on the phone |
| Copy **new** files and folders onto the phone | Editing a phone file in place (save-over from an editor) |
| Create folders; rename inside the same folder | Moving between folders / storages (copy, then delete) |
| Delete a file, or an empty folder | Deleting a non-empty folder in one step |
| Eject, unplug, plug in again | Spotlight search on the phone (indexing is switched off) |

Also good to know:

* **Works with Android phones in general.** MTPort speaks standard MTP, the protocol Android uses for USB file transfer.
  The only thing you have to do is choose **File transfer** in the phone's USB notification when you plug in the cable;
  after that it just works. If your phone behaves differently, please open an issue
  ([docs/DEVICE_COMPATIBILITY.md](docs/DEVICE_COMPATIBILITY.md) lists what has been checked so far).
* Speed is set by the phone's USB link, not by MTPort: about 17–33 MB/s on a USB 2.0 "High Speed" link, ~1 MB/s if the
  phone negotiates "Full Speed". There is no artificial speed limit.
* The end of a **very large** file (for example a 1.6 GB video) can only be read once macOS has downloaded that far, so
  previewing such a video can take about a minute. See [Troubleshooting](docs/TROUBLESHOOTING.md#big-videos-open-slowly).
* Sleep/wake while a phone is mounted has not been tested on hardware.

## Requirements

* A Mac with **macOS 15.4 or newer** (tested on macOS 26, Apple Silicon; Intel is untested).
* **Xcode** (to build the app; the free version from the App Store is fine) and [Homebrew](https://brew.sh).
* An Android phone, a **data-capable** USB cable, and the phone unlocked in *File transfer (MTP)* mode.
* macFUSE is **not** needed. (It is only used by an optional, experimental transport; see below.)

## Install

```bash
git clone https://github.com/oguzhanaktas1/MTPort.git
cd MTPort
brew install libmtp libusb cmake ninja pkgconf xcodegen
scripts/install-app.sh
```

`scripts/install-app.sh` builds the native tools and the app and installs **`~/Applications/MTPort.app`** (no `sudo`).
It takes a few minutes. Use `scripts/bootstrap.sh` first if you want to see what is missing; it only prints, it never
installs anything.

Because you build it yourself, macOS does not quarantine the app. If you copy a built `MTPort.app` from another Mac, right-click
it → *Open*, or run `xattr -dr com.apple.quarantine MTPort.app`.

## Use it

1. **Unlock the phone.** Pull down the notification shade, tap the USB notification, and choose **File transfer (MTP)**.
   If the phone asks to *allow access to data* or *allow this accessory*, accept.
2. Open **MTPort** (Dock icon or menu bar). Keep it running; optionally turn on *Launch at login* in its menu.
3. Plug in the cable. After a few seconds the phone appears in Finder (sidebar → *Locations*) and a Finder window opens.
   First connection after a plug-in can take up to a minute while the phone makes its storage available.
4. When you are done, **eject** it in Finder (or in MTPort) before unplugging. After an eject the phone is *not*
   mounted again until you unplug and re-plug it (or press *Mount*).

If the sidebar does not show it, enable *Finder → Settings → Sidebar → Locations → Connected servers*, and see
[Troubleshooting](docs/TROUBLESHOOTING.md). The phone's storage appears as a folder named after the phone's storage
(for example *Internal storage* or *Dahili depolama*).

### Command line

The same engine is available as `mtport` (build output: `build/native/cli/mtport`):

```bash
mtport doctor                      # what works on this Mac and what is missing
mtport devices                     # connected MTP phones
mtport info                        # storages and capabilities
mtport ls   --path /DCIM           # list a folder (paths are inside the phone's storage)
mtport pull --src /DCIM/Camera/IMG.jpg --dst ~/Desktop/IMG.jpg      # prints a SHA-256
mtport push --src ~/Desktop/test.bin --dst /Download/test.bin --verify
mtport mount [--read-write]        # mount without the app (read-only unless --read-write)
mtport unmount
mtport status
```

`push`, `mkdir` and `mv` never replace an existing object, and `rm` removes exactly one object (a folder only if it is
empty).

## How it works

```
MTPort.app (SwiftUI, menu bar + window)
   │  JSON over a private Unix socket
mtport-agent ── watches USB, starts one worker per phone
   │
mtport-mount (one per phone)
   ├─ owns the only MTP session (libmtp / libusb), serialises everything
   ├─ filesystem layer: cache, write staging on the Mac, safety rules
   └─ tiny WebDAV server on 127.0.0.1 (random port, random password)
         │
   macOS network-volume support (NetFS)  →  /Volumes/MTPort-<model>  →  Finder
```

* The phone is mounted the same way as *Finder → Connect to Server*, so it is a real network volume: it appears in the
  sidebar, has an eject button, and needs no kernel extension, no macFUSE and no admin password.
* The sidebar entry carries the **phone's own name**. macOS shows the host name of the mount URL, so the worker
  publishes that name for the loopback address (`127.0.0.1`) through the system's mDNS service. Nothing but your own
  Mac can use it.
* Files you copy **to** the phone are first written to a private staging area on the Mac and sent when the file is
  closed. If an upload fails the staged bytes are kept (`~/Library/Caches/MTPort/Staging`), never deleted silently.
* Finder's helper files (`.DS_Store`, `._*`) are kept in memory only and never written to the phone.

An experimental alternative, `mtport mount --transport fskit`, uses macFUSE 5.4+ with its FSKit backend. It is
not the default because macFUSE volumes are not listed in the Finder sidebar. Details in
[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Troubleshooting (short version)

| Symptom | Try |
|---|---|
| Phone is listed but the folder is empty, or "Operation not permitted", or the app says *Waiting for the phone* | Unlock the phone, choose **File transfer (MTP)**, accept the *allow access* prompt, unplug and re-plug with the screen on |
| Nothing appears in Finder | Is MTPort running? Check *Finder → Settings → Sidebar → Connected servers*; run `mtport doctor` and `mtport status` |
| `DeviceBusy: could not open an MTP session` | Quit Android File Transfer, OpenMTP, MacDroid, Image Capture, Photos; re-plug |
| `libusb_open() failed … Operation timed out`, phone vanishes from USB | Re-plug the cable (try another port/cable) |
| "Operation not supported" when saving over a file | By design: MTP cannot edit in place. Copy a new file instead |

The full list, with causes and fixes: **[docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md)**.

## Uninstall

```bash
osascript -e 'tell application "MTPort" to quit'
rm -rf ~/Applications/MTPort.app
ls ~/Library/Caches/MTPort/Staging        # anything in here is an upload that did not reach the phone
rm -rf ~/Library/Application\ Support/MTPort ~/Library/Caches/MTPort
```

## Project layout

| Path | What |
|---|---|
| `native/core` | MTP device interface, typed errors, `libmtp` backend, single-owner lock |
| `native/filesystem` | MTP-to-POSIX mapping, caches, write staging, safety rules |
| `native/webdav` | loopback WebDAV server and the macOS mount call |
| `native/fuse` | experimental macFUSE adapter |
| `native/mount`, `native/agent`, `native/cli` | `mtport-mount`, `mtport-agent`, `mtport` |
| `macos/MTPort` | SwiftUI app (project generated by `xcodegen`) |
| `tests` | unit/integration tests with a fake phone, plus scripts for real-phone checks |

See [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) to build and test, and [CONTRIBUTING.md](CONTRIBUTING.md) to help. The
most useful contribution: **try it on your phone and report the result**
([docs/HARDWARE_ACCEPTANCE.md](docs/HARDWARE_ACCEPTANCE.md)).

## License

[MIT](LICENSE). MTPort links to `libmtp` and `libusb` (LGPL-2.1+) dynamically and does not copy their code; see
[third_party/NOTICES.md](third_party/NOTICES.md). "Android" is a trademark of Google LLC; this project is not affiliated
with or endorsed by Google, Samsung, or Apple.
