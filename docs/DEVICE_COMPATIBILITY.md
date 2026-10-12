# Device compatibility

Only one phone has been tested so far. **Please add yours**: run [HARDWARE_ACCEPTANCE.md](HARDWARE_ACCEPTANCE.md) and send a pull
request (or an issue) with a new row. Leave a cell as `NOT RUN` rather than guessing.

| Device | Android | Cable/port | USB speed | `GetPartialObject` | Rename | List 1.8k files | CLI pull sha256 | Finder read | Finder write | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| Samsung Galaxy S21 FE (SM-G990E), macOS 26 / Apple Silicon | not recorded | USB-C cable to a Mac port | High Speed (USB 2.0) on the final runs; Full Speed on some earlier plug-ins | PASS | PASS (same folder) | PASS: first open 4–8 s, repeat < 1 s | PASS | PASS (123 of 123 cold copies byte-identical) | PASS (create/copy, SHA-256 round trips, refusals) | Phone needs the screen unlocked and "File transfer" each time it is plugged in; storage can take 10–60 s to become readable after a re-plug |

## What to expect (from the MTP spec and libmtp, not from tests on other phones)

* Most Android responders support `GetPartialObject`/`GetPartialObject64` → random-access reads (Quick Look, scrubbing).
  Without it the mount falls back to one staged full download per file (needs Mac disk space; refuses files over the
  read-cache budget with `EFBIG`).
* Many devices cap object sizes or lack 64-bit partial reads for >4 GiB; the code uses 64-bit offsets everywhere and
  fails explicitly rather than wrapping.
* Rename depends on the `ObjectFileName` property; after every rename the phone is re-read to prove it was applied.
* The phone must be unlocked; some Android versions only expose MTP after you pick *File transfer* each time you plug in.
* Samsung/Xiaomi/other vendors sometimes need the stock USB mode selected; vendor-specific acceleration is out of scope.
