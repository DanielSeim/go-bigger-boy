# Changelog

## [0.29.0] - 2026-09-05

### Dynamic plug-in ABI

- Formally approve and freeze the v1.0 fixed-width C plug-in contract,
  including numeric identifiers, 64-bit table layouts, ownership/error rules,
  and cross-toolchain compatibility requirements.
- Record the freeze baseline and change-control policy in
  [`docs/plugin-abi-freeze.md`](docs/plugin-abi-freeze.md). Automatic discovery
  remains a separate, opt-in security and UX project.

## [0.28.0] - 2026-09-04

This minor release consolidates the post-0.27.7 architecture, diagnostics, and
testing improvements.

### Architecture and diagnostics

- Split SDL event, storage, update, capability, and asynchronous callback
  boundaries further so optional Game Boy tools cannot be dispatched through an
  incompatible core.
- Preserve logging context across desktop file dialogs, Android JNI requests,
  and update-check/download worker callbacks.
- Add focused logging and SDL capability contract coverage.

### Testing and CI

- Add reproducible parser-fuzz campaigns, reviewed seed management, corpus
  checksum validation, and explicit corpus-promotion tooling.
- Extend the ThreadSanitizer job to build the SDL frontend, run the complete
  SDL-enabled contract suite, and perform a bounded Xvfb dashboard smoke test.
- Document the architecture audit and the remaining intentionally deferred
  work.

## [0.27.7] - 2026-09-03

This patch release fixes automatic display colors for SGB-capable DMG games.

### Fixes and accuracy

- Apply the selected automatic cartridge compatibility palette while an SGB
  game is still using its neutral startup palette.
- Preserve native SGB colors as soon as the game sends a `PAL` command.
- Add regression coverage for display settings on the SGB startup path.

## [0.27.6] - 2026-09-03

This patch release fixes Android display-palette restoration and extends the
diagnostic SGB command path.

### Fixes and accuracy

- Reapply the selected Android display palette after save-state and lifecycle
  restores, including the Game Boy Color automatic compatibility palette.
- Add deterministic SGB `CHR_TRN`/`PCT_TRN` transfer latches and `MASK_EN`
  viewport modes, with save-state persistence and focused core coverage.
- Extend opt-in PPU diagnostics with fetched tile, row, and bitplane data.

## [0.27.5] - 2026-09-02

This patch release fixes Android palette application and touch-control
retention while a game is running.

### Fixes

- Apply Android display-palette changes to the active emulator immediately.
- Preserve touch controls while navigating settings and returning to a game.
- Add regression coverage for Android settings and touch behavior.

## [0.27.4] - 2026-09-02

This release adds baseline Super Game Boy support.

### Super Game Boy

- Add SGB model selection, joypad polling, command handling, palettes, border
  state, mask modes, and save-state persistence.
- Add focused SGB accuracy and regression coverage.

## [0.27.3] - 2026-09-02

This patch release makes the Android voxel-orbit preference effective in the
running emulator.

### Fixes

- Honor the voxel touch-orbit toggle in the native SDL touch path.

## [0.27.2] - 2026-09-02

This patch release refreshes Android settings while a game remains open.

### Fixes

- Apply changed graphics, touch-layout, and menu-overlay settings without
  requiring an emulator restart.

## [0.27.1] - 2026-09-02

This patch release fixes Android menu-overlay integration and desktop update
flows.

### Fixes and desktop UX

- Fix the Android menu overlay build and apply its configured placement.
- Improve desktop download/editing flows and dashboard navigation.
- Fix Windows dashboard startup-update ordering.

## [0.27.0] - 2026-09-02

This release substantially improves desktop dashboard and tool usability.

### Desktop UI

- Add scalable text rendering, responsive dashboard scrolling, resizable
  Windows dashboards, artwork-download progress, and clearer tool-button
  feedback.
- Add a headless desktop frontend smoke test and improve dashboard navigation
  reliability.
- Fix Windows manifest/resource collisions and Android SDL build guards.

## [0.26.3] - 2026-09-02

This patch release adds configurable voxel camera controls on Android.

### Android

- Add a setting to enable or disable touch orbiting in voxel video modes.

## [0.26.2] - 2026-09-02

This patch release exposes voxel video modes in Android settings.

### Android

- Allow Android users to select the available voxel presentation modes.

## [0.26.1] - 2026-09-02

This patch release removes the redundant framework title bar from the Android
dashboard.

## [0.26.0] - 2026-09-02

This release improves Android navigation and advances audio and link-session
accuracy.

### Android UX

- Improve library/settings navigation, touch-layout editing, system-bar
  handling, and in-game menu access.

### Audio and link diagnostics

- Refine APU frame-sequencer, channel-startup, square-wave, and CGB volume-write
  timing.
- Add DMG/CGB waveform fixtures and opt-in SameSuite coverage.
- Improve link trace timestamps, progress diagnostics, and focused trade
  coverage.

## [0.25.2] - 2026-08-31

This patch release hardens link timeout recovery and state validation.

### Link cable

- Protect stalled-session recovery and validate loaded link states before
  running a scenario.
- Add regression coverage for timeout and save-state failures.

## [0.25.1] - 2026-08-30

This patch release stabilizes real-ROM local link sessions and introduces the
first scripted trade/battle integration harness.

### Link cable

- Recover stalled local sessions and preserve Pokémon serial-handshake state.
- Add deterministic real-ROM trade and battle scenarios with semantic outcome
  checks, per-frame traces, and CPU/game context diagnostics.
- Improve Cable Club automation and document the retry workflow for asymmetric
  host/join timing.

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
