# Security

MTPort runs entirely on your Mac: no cloud, no telemetry, no listening network service reachable from outside.

* The mount worker serves the phone to macOS over HTTP on **127.0.0.1 only**, on a random port, protected by HTTP Basic
  authentication with a random per-mount password that only the worker and the macOS mount call know.
* The agent's control socket is `0600` inside a `0700` directory and checks the peer's user id.
* Staging directories are `0700`, staged files `0600`.
* Names and sizes coming from the phone are treated as untrusted (UTF-8 validated, traversal-proof, 64-bit safe).
* Product code never calls `LIBMTP_Format_Storage` or `LIBMTP_Reset_Device` (checked by `tests/scripts/check_forbidden_api.sh`).

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** ("Security" tab → "Report a vulnerability") instead of a public
issue. Include the macOS version, how to reproduce, and what an attacker could do.

Side note: the project is a developer preview. It is not signed or notarized, and it has been tested with very few phones
(see [docs/DEVICE_COMPATIBILITY.md](docs/DEVICE_COMPATIBILITY.md)).
