# Benchmarks

Numbers below come from one phone. Numbers produced by `--backend fake` measure the Mac's disk, not MTP, and must never be quoted.

MTPort applies **no bandwidth throttle or artificial delay**. Serialization of MTP calls, disk budgets and queue limits
are stability policies. The read chunk (default 16 MiB, small reads stay small) was chosen from the measurements below, not assumed.

## Method (same phone, cable, USB port, file set)

| Variant | Command |
|---|---|
| libmtp baseline | `mtp-getfile <id> /tmp/out` (from libmtp) |
| MTPort CLI, whole-object | `mtport bench --direction pull --file /Download/benchmark.bin --mode download --runs 3 --json` |
| MTPort CLI, partial reads | `mtport bench --direction pull --file ... --mode partial --chunk-kib {256,1024,4096,8192} --runs 3 --json` |
| Finder / FUSE | `dd if=/Volumes/MTPort-X/.../benchmark.bin of=/dev/null bs=1m` and a Finder copy (stopwatch) |
| Windows Explorer | only if a Windows baseline exists |

Record for each run: phone model/Android, negotiated USB speed (`system_profiler SPUSBDataType`), macOS/macFUSE/libmtp versions,
file sizes (1 × 1 GiB, 1 × 4+ GiB if safe, 1000 small files), directory first-open vs second-open latency, peak RSS/CPU
(`/usr/bin/time -l`), staging disk used, errors, and SHA-256 of every downloaded copy. `bench` never writes to or deletes from the phone.

## Results

| Date | Phone | Variant | Size | Seconds | MiB/s | sha256 ok | Notes |
|---|---|---|---|---|---|---|---|
| 2026-10-11 | Samsung SM-G990E | CLI whole-object download | 97.3 MiB | 90.5 | 1.08 | yes (self-consistent) | USB negotiated **Full Speed 12 Mbit/s** (IORegistry `Device Speed = 1`); link-limited |
| 2026-10-11 | Samsung SM-G990E | CLI partial 256 KiB / 1 MiB / 4 MiB (first 32 MiB) | 32 MiB | 31.9 / 30.2 / 30.3 | 1.00 / 1.06 / 1.06 | yes | same slow link; chunk size irrelevant here |
| 2026-10-12 | Samsung SM-G990E | raw libmtp `GetPartialObject`, 8 s each, different file regions | 125–285 MB | 8 | 15.6 (256 KiB) / 24.3 (1 MiB) / 24.8 (4 MiB) / 33.6 (16 MiB) | – | **USB High Speed** (`Device Speed = 2`); the fixed per-request cost of the phone dominates, so bigger requests are faster |
| 2026-10-12 | Samsung SM-G990E | HTTP stream through the worker, old (1 MiB) vs new (16 MiB) chunks, same three 200 MB regions, 6 runs | 200 MB each | – | ~17.5 -> ~33 on the later regions (~11 -> ~17 on the first 100-300 MB) | – | High Speed |
| 2026-10-12 | Samsung SM-G990E | cold read at EOF-100 of a video through the Finder volume | 62.7 MB / 1.62 GB | 2.5 / 50.9 | – | – | the macOS client waits for its own download (see docs/TROUBLESHOOTING.md) |
| 2026-10-12 | Samsung SM-G990E | first listing of `DCIM/Camera` (1,853 entries), per-object `GetObjPropList` vs `GetObjectInfo` | 1,856 objects | 20.3–40 / 10.3 | – | – | identical metadata (0 differences); first listing after mount 35–48 s -> 4–8 s |
