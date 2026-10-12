# Third-party notices

MTPort is original code. It *links to / depends on* the following; none of their source is copied into this repository.

| Component | Use | License | Notes |
|---|---|---|---|
| libmtp 1.1.x | MTP initiator (`LibmtpDevice`) | LGPL-2.1-or-later | Linked dynamically from Homebrew. Keep it replaceable if you redistribute binaries; ship the license text and source offer. |
| libusb 1.0.x | USB transport used by libmtp | LGPL-2.1-or-later | Not accessed directly by MTPort (no second handle behind libmtp). |
| macFUSE 5.4+ (libfuse 2.9 API, FSKit backend) | optional experimental Finder mount (`--transport fskit`); the default WebDAV transport does not use it | macFUSE license (see https://github.com/macfuse/macfuse/blob/main/LICENSE.txt; libfuse portions LGPL-2.1) | User-installed runtime; **not bundled** and not redistributed here. Only its public headers were used for compile checks. |
| Apple system frameworks (CommonCrypto, CoreFoundation, IOKit, SwiftUI, ServiceManagement) | SHA-256, Unicode normalization, USB registry, UI | Apple SDK license | – |
| xcodegen (dev tool only) | generates `MTPort.xcodeproj` | MIT | Not shipped. |

Reference-only (inspected for behaviour, **not copied**): `su-z/simple-mtpfs-mac` (GPL-2.0), macFUSE demos, libmtp examples, Apple's
open-source `webdavfs` (read to understand the macOS WebDAV client's behaviour).

MTPort itself is released under the [MIT License](../LICENSE). Linking dynamically to LGPL libraries (libmtp, libusb) from MIT code is
fine; if you redistribute a *binary* build, keep those libraries replaceable and include their license texts.
