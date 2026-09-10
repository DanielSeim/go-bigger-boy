# Changelog

## [Unreleased]

### Super Game Boy

- Decode retained `CHR_TRN`/`PCT_TRN` data into a deterministic 256×224 SGB
  border framebuffer, including SNES 4bpp tiles, tilemap flips, RGB555 border
  palettes, and transparent overlay of the native Game Boy viewport.
- Expose the SGB framebuffer through the core, emulator, and memory-bus APIs.
- Route SGB/SGB2 frames through the frontend-neutral video contract so SDL,
  Android, and web presentation use the 256×224 dimensions automatically.
- Persist the PCT transfer latch in save-state format version 27 and add
  compositor regression coverage.

## [0.33.14] - 2026-09-09

### Android settings and diagnostics

- Avoid showing the hardware-model restart toast when merely opening Settings.
- Apply newly enabled link diagnostics immediately to the running Android
  frontend, so traces can be captured without restarting the emulator.

## [0.33.13] - 2026-09-09

### Android link diagnostics

- Add an Android Link settings toggle for opt-in link tracing.
- Add an in-game **Save diagnostics** action that exports the active or most
  recently completed trace through the system file picker.
- Keep Android traces in durable app-private storage and add snapshot coverage
  for active and stopped sessions.

## [0.33.12] - 2026-09-08

### Cross-platform frame pacing

- Optimize PPU object-pixel deadline tracking to remove a full-screen scan from
  every Mode 3 dot, reducing CPU work in sprite-heavy games such as Pokémon.
- Schedule rewind snapshots inside the frame pacer's idle interval and defer
  captures when a frame has insufficient headroom, avoiding visible Windows
  and Android stutter.

### Web hardware model selection

- Expose the shared DMG, MGB, SGB, and CGB hardware-model catalog in the
  browser frontend.
- Persist the selected model in browser storage and apply it when the next ROM
  is loaded, matching desktop and Android behavior.

## [0.33.11] - 2026-09-07

### Android Play closed beta publishing

- Publish tagged Android App Bundles automatically to the configured `GBB Beta`
  closed-testing track through GitHub Actions Workload Identity Federation.
- Document the closed-beta release path and keep direct-download updater
  behavior unchanged.

## [0.33.10] - 2026-09-07

### Android data portability

- Add full backup and restore through Android's system file picker.
- Add save-file ZIP export and individual `.sav` import while preserving
  fingerprint-based filenames.
- Validate backup archive paths and size limits before writing app data.

## [0.33.9] - 2026-09-07

### Android Play updates

- Add Google Play flexible in-app update prompts for Play-installed builds;
  updates are downloaded and completed by Google Play while direct APK builds
  continue using the verified GitHub updater.

## [0.33.8] - 2026-09-07

### Android launcher identity

- Use adaptive `mipmap` launcher icons in the Android manifest so system
  update surfaces show the Go Bigger Boy artwork instead of a generic icon.
- Keep density-qualified fallbacks for older Android versions and isolate the
  monochrome layer to Android 13 and newer.

## [0.33.7] - 2026-09-07

### Android updater reliability

- Recognize Google Play installations through installing, initiating,
  originating, and update-owner metadata before selecting the direct GitHub
  APK update channel.
- Re-evaluate the install source for each update check and log the metadata
  used for the decision, covering Play updates of previously sideloaded builds.
- Add regression coverage for every supported Play installer metadata path.

## [0.33.6] - 2026-09-07

### Android Play publishing

- Delegate update discovery for Play-installed Android builds to Google Play
  while retaining the verified GitHub APK updater for direct downloads.
- Publish signed tagged Android App Bundles automatically to the Play internal
  testing track through Workload Identity Federation.
- Pin Gradle Play Publisher to the Gradle 8.13-compatible 3.13.0 release and
  validate the Play publishing configuration in CI.

## [0.33.5] - 2026-09-07

### Android Play internal testing

- Publish a new patch version for validating Google Play internal-test updates
  and the Play-signed Android package distribution path.

## [0.33.4] - 2026-09-07

### Link performance and battle stability

- Reduce Windows frame jitter during remote byte-link sessions by using a
  coarse cadence while a receiver is passive and a low-latency cadence only
  while a response is pending.
- Re-evaluate the serial clock direction at each emulation slice so CGB fast
  mode and clock handoffs cannot inherit a stale polling interval.

## [0.33.3] - 2026-09-07

### Link handshake reliability

- Restore the established byte transport during mixed Gen I/Gen II Cable Club
  entry so Crystal and Gen I games remain in the same reserved/waiting state.
- Retain bounded deferred-request recovery and its diagnostics for peers that
  stop arming their serial receiver.

## [0.33.2] - 2026-09-07

### Link transport reliability

- Bound deferred serial requests and return a retryable `not-ready` response
  when a peer stops arming its receiver, preventing both guests from waiting
  indefinitely.
- Extend link diagnostics with byte-path eligibility and deferred-request
  polling state, and add regression coverage for both safeguards.

## [0.33.1] - 2026-09-06

### Link compatibility

- Recognize the compact international Pokémon Gold, Silver, and Crystal CGB
  headers so Gen I/Gen II Time Capsule sessions negotiate correctly.
- Add regression coverage for the retail Gen II header aliases and shared
  Western compatibility profile.

## [0.33.0] - 2026-09-06

### CGB revision accuracy

- Add a pinned SameBoy v1.0.3 revision matrix for CGB-0, CGB-C, and CGB-E
  boundary behavior.
- Model revision-specific CGB APU envelope writes, square retrigger alignment,
  and channel-4 startup timing, with focused hardware-contract coverage.
- Add repeatable SameBoy register-boundary probes and reviewed three-take
  summaries for PCM visibility and noise LFSR reload diagnostics.
- Keep external reference conversion provenance and the accuracy report in
  sync with the reviewed fixtures.

## [0.32.0] - 2026-09-06

### CGB timing and accuracy

- Preserve the CGB APU's 1 MHz phase across normal-speed execution, double-speed
  switches, split bus ticks, and save-state restores.
- Add contract coverage and diagnostics for the CGB APU phase boundary.
- Document the CGB phase behavior and refresh the accuracy status counts.

## [0.31.13] - 2026-09-06

### Link audio and scheduling

- Prebuffer SDL audio before playback and re-prime it after ROM/state resets,
  preventing short link-polling delays from becoming audible underruns.
- Tighten passive byte-receive polling on Android to keep active battle links
  responsive across LAN and Bluetooth sessions.
- Extend serial link regression coverage with a sustained alternating payload
  exchange to catch ownership drift after long battles.

## [0.31.12] - 2026-09-06

### Link cable reliability

- Preserve a joiner's pending internal-clock request while the host finishes
  the current byte, preventing Android/Windows Cable Club handoff desyncs.
- Clear completed host-side arbitration denials so a stale busy flag cannot
  block subsequent link transfers.
- Add regression coverage for the cross-frame clock-ownership handoff race.

## [0.31.11] - 2026-09-06

### Link cable performance and diagnostics

- Reduce polling work on passive byte-capable receivers while keeping the
  host-clock response path on the low-latency cadence. This avoids unnecessary
  Windows frame-time jitter during LAN/Bluetooth sessions.
- Make Pokémon link diagnostics bank-aware so localized WRAM state is reported
  correctly for German and other European ROMs.

## [0.31.10] - 2026-09-06

### Link cable performance

- Add an idle-link execution fast path so an established but inactive remote
  session does not add per-frame scheduling overhead on Windows.
- Keep active serial transfers on the bounded low-latency polling cadence when
  they begin during an idle slice.

## [0.31.9] - 2026-09-06

### Link cable performance

- Run connected remote emulation in bounded cycle slices instead of a
  per-instruction frontend loop, preserving serial polling deadlines while
  reducing Windows joiner frame jitter.
- Avoid redundant frame-pacer socket polling for negotiated byte-level links
  and keep the legacy bit-level polling cadence unchanged.

## [0.31.8] - 2026-09-06

### Link cable discovery and performance

- Make LAN discovery resilient when broadcast or multicast traffic is
  filtered: hosts advertise periodically, Windows scanners probe local
  subnets with unicast UDP, and scanners listen on the discovery port when
  available.
- Enable broadcast delivery on host discovery sockets so Android-hosted
  sessions can be found automatically from Windows.
- Avoid unnecessary remote-link socket polling while idle and reduce legacy
  bit-link polling overhead to improve Windows frame pacing.

## [0.31.7] - 2026-09-06

### Link cable discovery and performance

- Add Windows adapter-specific subnet broadcast queries for LAN discovery when
  limited broadcast traffic is filtered by the local network.
- Skip rewind snapshot serialization during remote link sessions so it cannot
  interrupt emulation frame pacing.
- Link the Windows discovery implementation with the system IP Helper API.

## [0.31.6] - 2026-09-06

### Link cable discovery and performance

- Join LAN discovery multicast on every active Android/Linux IPv4 interface,
  while retaining an API-21-safe Android fallback.
- Extend the Windows discovery retry window to tolerate delayed Android Wi-Fi
  multicast setup.
- Reduce socket polling overhead for negotiated byte-level serial links to
  improve Windows frame pacing without changing legacy bit-link timing.

## [0.31.5] - 2026-09-06

### Link cable performance

- Negotiate a byte-level TCP/Bluetooth serial fast path to eliminate seven
  network round trips per transferred byte while retaining fallback support
  for older peers.
- Add byte-path negotiation and packet counters to link diagnostics and
  integration traces.
- Extend link codec and endpoint regression coverage for complete byte
  exchanges.

## [0.31.4] - 2026-09-06

### Link cable discovery

- Add multicast LAN discovery alongside broadcast and loopback fallback paths,
  improving Windows discovery of Android-hosted TCP sessions.
- Bind the discovery socket before joining the multicast group for reliable
  delivery on Android and Linux network stacks.

## [0.31.3] - 2026-09-06

### Link cable reliability

- Restore the Windows Game Library menu action after the SDL event callback
  ordering was corrected.
- Improve Android LAN host discovery by enabling scoped Wi-Fi multicast
  reception and handling Android 16 local-network permission requirements.

## [0.31.2] - 2026-09-06

### Frontend architecture and link settings

- Extract SDL emulation-mode selection into a core-independent policy contract
  with standalone coverage for pause, rewind, local link, replay, remote link,
  and fast-forward precedence.
- Add complete TCP, LAN-discovery, Bluetooth, and diagnostics settings to the
  Windows dashboard and Android settings flow, with transport-specific fields
  and persistence.
- Reduce the default Windows dashboard height so it remains usable on 1080p
  displays.

## [0.31.1] - 2026-09-06

### Bluetooth link cable

- Correct Windows RFCOMM SDP service removal so restarting a host does not
  leave a stale Bluetooth advertisement behind.
- Populate the complete RFCOMM service metadata required by Windows Bluetooth
  adapters when registering the host service.

## [0.31.0] - 2026-09-06

### Bluetooth link cable

- Generalize the serial endpoint around a packet-channel boundary shared by
  TCP and Bluetooth transports.
- Add Bluetooth Classic RFCOMM framing on Windows, including SDP service
  registration, and an Android worker-backed RFCOMM bridge.
- Add Bluetooth transport, device address, and service UUID settings plus the
  Android nearby-device permissions and setup guidance.

## [0.30.6] - 2026-09-05

### Android LAN link

- Keep Android link-popup hit targets aligned with the rendered menu rows so
  Discover cannot trigger the library action.
- Retry LAN discovery broadcasts during a two-second scan window to tolerate
  dropped Wi-Fi broadcasts and delayed firewall delivery.
- Automatically use a reachable wildcard bind when LAN discovery is enabled
  but the legacy loopback default is still configured.
- Explain the required host advertisement and UDP firewall configuration when
  no compatible LAN host is found.

## [0.30.5] - 2026-09-05

### SDL frontend

- Keep the Android in-game menu open after a tap and improve its touch
  readability.
- Show the remote transport status and counters while a Windows host is
  listening, before a peer connects.
- Answer LAN discovery queries from the active link session so Android peers
  can discover Windows hosts.
- Make slow-core fast-forward provide a visible speed increase and avoid
  normal frame pacing while fast-forwarding.

## [0.30.4] - 2026-09-05

### Android link controls

- Add an in-game link button and popup for TCP host, join, LAN discovery,
  retry, stop, and link settings actions.
- Add an Android link-settings dialog so connection values can be adjusted
  without leaving the running game.

### Frontend performance

- Adapt fast-forward batching and audio reduction to the measured core cost,
  keeping slower Windows systems responsive while retaining up to 4× speed.
- Skip rewind captures during fast-forward and reduce pending TCP polling and
  status rendering overhead while a host is waiting for its peer.
- Use the concrete built-in emulator frame-step path to avoid unnecessary
  dispatch overhead.

### Windows updater

- Prevent the hidden native dashboard window from flashing before the update
  prompt appears.

## [0.30.3] - 2026-09-05

### Frontend performance

- Keep a TCP host on the efficient emulation path while it is waiting for a
  peer, avoiding excessive non-blocking socket polling.
- Skip expensive rewind snapshots during fast-forward while preserving the
  existing rewind history.
- Buffer link diagnostics between periodic flushes instead of forcing disk I/O
  after every emulated frame.

## [0.30.2] - 2026-09-05

### Windows updater

- Poll for updates while the native game library dashboard is open, so update
  notifications no longer wait until a ROM is selected.

## [0.30.1] - 2026-09-05

### Windows performance

- Prefer the Direct3D11 renderer on Windows, with a safe software fallback.
- Improve Windows frame pacing and request high-resolution timer support.
- Cache native menu state so unchanged menus do not trigger repeated Win32
  updates.
- Enable release link-time optimization for the emulator core and frontend.
- Reduce rewind snapshot overhead with preallocated state buffers, a faster
  CRC32 pass, and amortized snapshot capture.
- Add opt-in frame timing diagnostics for separating event, emulation, audio,
  presentation, pacing, core-step, and rewind costs.

## [0.30.0] - 2026-09-05

### Android LAN link cable

- Add Android host, join, retry, stop, and LAN discovery actions to the
  in-game touch menu.
- Add Android settings for the remote host, bind address, TCP port, and LAN
  discovery advertisement.
- Share the TCP serial endpoint and compatibility checks between Windows and
  Android, including safe teardown when leaving a running game.
- Document Windows-to-Android and Android-to-Android LAN setup.

### Plug-in integration

- Add an explicit, settings-controlled native plug-in catalog with deterministic
  path/directory handling, duplicate suppression, load diagnostics, and a hard
  discovery limit. Plug-ins remain disabled by default and are never loaded on
  Android.
- Add desktop Settings controls for enabling discovery and requiring an
  identity allowlist, with restart-required messaging and visible load/rejection
  status.
- Add an explicit capability permission allowlist with stable capability names,
  unknown-name rejection, desktop settings support, and discovery regression
  coverage.
- Add a fail-closed pre-registration trust callback for host integrations,
  providing the enforcement boundary needed by a future interactive prompt.
- Add a portable SHA-256 file helper with standard-vector coverage for digest
  pinning and the future signed-manifest verifier.
- Define the canonical signed plug-in manifest, trust-store rotation, and
  fail-closed verification order for the remaining publisher-trust work.
- Extend the core registry with context-owning providers so dynamically loaded
  plug-ins stay alive for the lifetime of their adapted cores.

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
