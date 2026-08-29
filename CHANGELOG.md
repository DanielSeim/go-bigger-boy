# Changelog

## [0.25.0] - 2026-08-29

This release adds the first complete desktop link-cable session stack and
hardens Pokémon Cable Club synchronization for unevenly scheduled emulator
instances.

### Link cable

- Added a reusable `LinkTransport` boundary, versioned/checksummed packet
  framing, and `LinkSession` lifecycle and retry management.
- Added deterministic local two-emulator sessions with cycle-balanced
  scheduling, transfer progress tracking, timeout detection, and recovery
  that preserves both emulator saves.
- Added non-blocking TCP host/join channels on loopback, including partial
  writes, packet validation, connection polling, explicit clock arbitration,
  and a remote serial endpoint that never blocks emulation.
- Hardened Gen I handshake behavior when the two Cable Club attendants are
  approached at different times, including deferred first requests, clock
  release, sequence-numbered reset markers, asymmetric scheduling, repeated
  SB/SC probe writes, and retry recovery.
- Enabled TCP low-latency mode and 1 ms link polling during desktop frame
  pacing, substantially reducing per-bit request/response delay without
  changing emulation timing.
- Added SDL controls for local sessions, TCP host/join/stop, handshake retry,
  split-screen status, and per-session transfer diagnostics.
- Made link traces opt-in and writable to the OS temporary directory, with
  role-specific filenames and portable/working-directory fallbacks.

### Validation and documentation

- Added codec, transport, endpoint, session, retry, timeout, and asymmetric
  scheduling tests, including repeated stress runs.
- Made loopback endpoint tests tolerate normal cross-platform socket scheduling
  latency instead of assuming that a fixed number of tight polls completes the
  transport handshake.
- Documented the link architecture, TCP usage, retry workflow, diagnostics,
  and the release smoke-test checklist.
- Updated desktop and Android version metadata to 0.25.0.
