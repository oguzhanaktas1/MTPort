# Contributing

Thanks for helping! The most valuable contribution right now is **testing on more phones**: see
[docs/DEVICE_COMPATIBILITY.md](docs/DEVICE_COMPATIBILITY.md) and [docs/HARDWARE_ACCEPTANCE.md](docs/HARDWARE_ACCEPTANCE.md).

## Build and test

```bash
brew install libmtp libusb cmake ninja pkgconf xcodegen
scripts/build-native.sh --test          # debug build + all tests (fake phone, no hardware needed)
scripts/build-native.sh --asan --test   # AddressSanitizer + UBSan
tests/scripts/check_forbidden_api.sh .  # product code must never format/reset a device
```

Swift app: `cd macos/MTPort && xcodegen generate && xcodebuild -project MTPort.xcodeproj -scheme MTPort test`
(the test host app auto-mounts a connected phone; quit it afterwards).

More details: [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md).

## Ground rules (they protect people's photos)

* **Never lose or silently alter data on the phone.** No overwrite, no in-place edit, no recursive delete, no "delete then
  send". Unsupported operations must fail with a clear error, never pretend to succeed.
* One MTP session owner per phone. Use `libmtp` through the RAII wrappers; no second USB handle behind libmtp's back.
* No throttling, no telemetry, no network access beyond the loopback mount server, no root, no ADB.
* New behaviour needs a test against the fake phone (`tests/fakes`). Behaviour that only a real phone can show (timing,
  macOS client quirks) should come with the measurement in the pull request.
* Format with `.clang-format`; keep comments about *why*, not what.

## Pull requests

Describe what you tested and on which phone/macOS. Do not paste private file names or photos in issues or PRs.
