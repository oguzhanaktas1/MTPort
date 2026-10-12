# Data safety

The phone's data is never modified except by an action the user explicitly performed in Finder/CLI, and only
with operations whose result the phone confirmed.

## Guarantees implemented (tested against the fake phone; the main ones also on a real phone)

| Rule | Behaviour | Test |
|---|---|---|
| Read-only unless asked | `mtport mount` (CLI) is read-only; `--read-write` opts in. The app mounts read/write (writes are staged, see below) and its "mount on plug-in" switch can be turned off | `agent_core.*`, `fake_filesystem.mutations_are_refused_on_read_only_mount` |
| No false success | A staged write is uploaded on `flush`/`fsync`/`release`; the phone's object appears in listings only **after** the upload succeeded; failures are returned to `flush`/`fsync` (and repeat on every later call) | `fake_filesystem.staged_create_commits_only_after_flush`, `failed_upload_*` |
| Failed upload keeps data | Staged bytes + a `.manifest` stay in `~/Library/Caches/MTPort/Staging/`; never auto-deleted; a failed upload is never blindly retried | `staged_writes.failed_commit_*` |
| No overwrite | Existing file: `open(O_TRUNC)`, write, rename-onto, `cp` over → `EOPNOTSUPP`; CLI `push` → `FileAlreadyExists`. There is no delete-then-send path | `fake_filesystem.open_flags_*`, `rename_rules`, `upload_duplicate_*` |
| No in-place edit | Writes to files that already exist on the phone → `EOPNOTSUPP` (reads keep working, even if FSKit opens O_RDWR) | same |
| No recursive delete | `rmdir`/`rm --dir` of a non-empty folder → `ENOTEMPTY`; MTP `DeleteObject` on a folder can remove the subtree, so the emptiness check happens first | `fake_filesystem.mkdir_unlink_rmdir`, `cli_mock_roundtrip` |
| Exactly-one-object delete | `unlink` removes the resolved object id only; type must match (`EISDIR`/`ENOTDIR`) | `delete_touches_only_the_target_object` |
| No silent copy+delete | Cross-folder/storage `rename` → `EXDEV`; rename onto existing name → `EOPNOTSUPP`; after a rename the phone must report the new name or it is an error | `rename_rules` |
| Downloads are atomic | Data goes to a private temp file and is moved into place (`RENAME_EXCL`) only when complete and size-checked; existing targets are never replaced | CLI script |
| Unplug | All handles/ops answer `ENODEV`; staged bytes preserved; the worker unmounts; nothing is deleted or replayed | `unplug_during_*` |
| Sleep/wake | Session handed back, phone re-enumerated, reconnect only if still attached; old handles → `ESTALE` | `detach_releases_the_session...` (unit); wake path itself NOT RUN |
| Product code can't format/reset | `LIBMTP_Format_Storage`, `LIBMTP_Reset_Device` never referenced | `forbidden_api` |
| Untrusted metadata | UTF-8 validated, `/`, NUL, `.`/`..`, >255 bytes rejected or escaped reversibly; duplicate names get stable suffixes; 64-bit sizes/offsets, overflow rejected | `virtual_paths.*`, `fake_filesystem.sizes_above_4gib_*` |

## Deliberate deviations / things to know

* **Finder metadata files** (`.DS_Store`, `._*`, `.localized`, …) created on the volume are kept **in RAM only** (bounded) and never
  sent to the phone, so Finder copies don't litter the phone or fail on xattr fallbacks. They read back normally until unmount.
  `--metadata-passthrough` uploads them like any file. Creating `.Spotlight-V100`, `.fseventsd`, `.Trashes`, `.TemporaryItems`
  folders is refused (`EPERM`).
* **Real-phone coverage.** The write rules were exercised on one phone (Samsung Galaxy S21 FE): create/copy files of 0 bytes to 1 MiB with
  SHA-256 round trips, refused overwrite and in-place edit, rename in the same folder, refusal to delete a non-empty folder, and clean-up of
  exactly the test data. Unplug during a transfer, sleep/wake and disk-full were not exercised on hardware.
* `chmod`/`chown`/`utimens` on existing phone files fail (`EPERM`/`EOPNOTSUPP`) because MTP cannot honour them; `--accept-metadata-noops`
  turns them into silent no-ops if a tool insists. Finder may show a warning on copies that preserve attributes (unverified).
* `transfer.cancel` is not implemented: interrupting an MTP upload can leave a partial object on the phone, and we refuse to guess
  which object to clean up. Unmounting preserves staged data.
* A write file is committed at the **first** `flush`; later writes through the same handle fail (`EOPNOTSUPP`). Programs that
  `fsync` mid-way and keep writing will see that error rather than silent corruption.
* Listings: the folder's file list is re-read every time (new and deleted files show up within a few seconds), but the size/date of a file that
  already was in the list is remembered for up to 5 minutes. Opening or copying a file always asks the phone for its real size first.
* Mount-time cost is bounded: only `DCIM/Camera`, `Download` and `Pictures` are listed in the background after mounting (so the first visit is fast), nothing else is scanned; no gallery scan, no thumbnails, no index.

## Privacy

Local only: no telemetry, no analytics and no connection to anything but your own Mac (the phone is served to macOS over a loopback-only,
password-protected HTTP connection). Logs never contain file names, paths, EXIF or serial numbers
(`LIBMTP_Set_Debug(0)`, structured counters only). Device ids are `vendor:product@bus.address`, not serials.
Staging dirs are `0700`, files `0600`; the control socket is `0600` in a `0700` directory and checks the peer UID.
