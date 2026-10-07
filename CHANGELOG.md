# Changelog

## [Unreleased]

- Add isolated native E0/E5/ED instrument and volume controls for measured
  values, with full validation, inherited state and owned BRR audio. Match
  eighteen instrument/volume setup snapshots from six private SGB1/SGB2 runs;
  check reset/save-load, reduced-volume PCM and silent rejection. Keep broader
  controls, original timbre and bundled playback unqualified.

- Expand the isolated native gate renderer to ten measured profiles, including
  durations 8/24, tempo 128 and short articulation at tempo 192. Use bounded
  exact profile lookup, validate mixed/inherited state and silent rejection,
  and compare all profiles against twenty private SGB1/SGB2 timing runs with
  reset/save-load and owned PCM checks. Keep bundled firmware unchanged.

- Add owned duration/tempo gate timing fixtures for durations 8/24, tempo 128
  and short articulation at tempo 192, retaining a byte-identical baseline
  control. Validate parameter isolation, symbolic timelines and sixteen private
  SGB1/SGB2 timing observations; leave native support and bundled firmware
  unchanged until additional bounded profiles are validated.

- Add explicit calibrated duration-16 native articulation gates for the three
  measured tempo/articulation profiles. Validate DSP key-off timing, unchanged
  note spacing, active-gate save/load and fail-closed profile rejection against
  ROM-free checks and six private SGB1/SGB2 runs. Keep general timing laws and
  bundled vendor playback unqualified.

- Add an isolated native SPC DSP renderer for three measured pitches and an
  owned looping BRR waveform. Check instrument setup, deterministic centered
  PCM, reset/save-load continuation, rests/release and fail-closed validation;
  compare key-on spacing against six private SGB1/SGB2 reference cases. Keep
  original gates, instrument mapping and bundled playback unqualified.

- Connect an isolated bounded native SPC note/rest parser to the fractional
  score clock. Validate full streams before timing starts, align native events
  with the symbolic oracle and check reset/save-load timelines and six private
  timing references. Keep DSP rendering and production ownership deferred.

- Add an isolated original SPC fractional score clock for tempos 96/192, with
  native reset, save/load, low-byte carry and timer accumulation checks. Compare
  bounded duration-16 timing against six SGB1/SGB2 private-reference cases;
  keep the experiment outside bundled firmware and playback ownership.

- Add an independent bounded symbolic vendor scheduler for channel 2 or channels
  2/3, first-ending-track phrase transitions, clipped long notes and finite
  nonnested EF calls. Align owned fixture timelines with twelve original-program
  reference cases; reject ambiguous boundaries and unsupported grammar. Native
  SPC rendering and vendor playback remain separate, unimplemented work.

- Add owned finite subroutine fixtures and a bounded private original-firmware
  gate. Validate EF counts 1..3 as total body executions, return to a distinct
  caller continuation and duration-16 onset spacing across SGB1/SGB2, with
  sanitized reports and ROM-free checks. Nested calls and vendor playback
  remain unsupported.

- Add owned two-channel phrase fixtures and a bounded private original-firmware
  gate. Validate combined voice-2/3 key-ons and early phrase advancement for
  swapped unequal durations, with an equal-long-duration control on SGB1/SGB2.
  Preserve the separate GBS4 barrier contract and unsupported vendor playback.

- Add independently authored echo routing/setup fixtures and a bounded private
  original-firmware gate. Pin settled EON/volume/feedback/delay/allocation/FIR
  snapshots across SGB1/SGB2 before each note's first key-off, with explicit
  settling rests, fail-closed reporting and ROM-free checks; echoed PCM and
  vendor playback remain unqualified.

- Add independently authored pan and volume note fixtures and a bounded private
  original-firmware gate. Pin centered/endpoint pan and reduced song/track volume
  register setup across SGB1/SGB2, with ROM-free isolation, checksum, sanitization
  and failure tests; complete curves and vendor playback remain unsupported.

- Add owned three-note tempo/articulation fixtures and a bounded private
  original-firmware timing matrix. Validate double-tempo onset spacing and
  shorter articulation gates across SGB1/SGB2 using sanitized DSP write timing;
  retain explicit tolerances and unsupported vendor playback status.

- Add independently authored single-note cartridges and a bounded private
  original-firmware DSP reference gate. Pin channel-2 pitch/source/envelope
  setup for three notes and instruments 2/10 across SGB1/SGB2; validate transport,
  sanitization and fail-closed reporting without copying samples or enabling
  vendor score playback.

- Add an original empty-song cartridge and optional private-reference gate for
  vendor directory selection. Validate codes 1..3, reordered and repeated
  requests on SGB1/SGB2 originals using address-only observations. Add ROM-free
  checksum, transport, JOYP and fail-closed reporting checks; vendor score
  rendering and title/audio qualification remain unsupported.

- Add address-only observation of the first three candidate vendor song-table
  words to the native fractional APU trace helper. Bound stored reads, retain
  write-order and nonzero SOUND delivery metadata, and exclude score values.
  Validate real SPC dummy reads, filtering, overflow and CLI output protection;
  private two-model original runs support music code 1 selecting the first word
  at `$2B00`. Broader song selection and playback remain separate gates.

- Add bounded, metadata-only vendor score-demand inventory with explicit roots
  and a limited common N-SPC command-width profile. Stop at unknown commands,
  subroutines and phrase controls; keep title qualification false. Refresh the
  private two-model Donkey Kong transfer failure and document the concrete
  scheduler, instrument, echo and larger-score gaps without committing assets.

- Add deterministic inspection of canonical original GBS1..GBS6 score uploads,
  reporting phrase barriers, finite repeats, track endings, control defaults and
  effective pitches with held-tie semantics. Validate transport integrity and
  reject malformed or noncanonical input; reports remain static authoring
  evidence without playback or vendor-title qualification.

- Add original GBS6 per-track signed transpose with mailbox v10, limited to
  -12..12 semitones and validated effective pitch indices 0..31. Preserve held
  pitch on ties, reset transpose on phrase/repeat/restart boundaries, and reject
  unsupported older envelopes before readiness. Validate real two-model octave,
  tie, independent-track, endpoint, scalar audio, reset and save/load behavior.
  Vendor title formats remain unsupported.

- Add original GBS5 bounded phrase-sequence repeats with mailbox v9: require one
  to four total plays, validate all patterns before readiness, restore defaults
  on each pass and preserve the repeat counter in save states. Validate real
  two-model repeat pitch/gain, stop/restart, finite and immediate transitions,
  scalar audio, reset, restore and existing score-format behavior.
  Vendor title formats remain unsupported.

- Add original GBS4 finite phrase sequencing with mailbox v8: validate one to four
  two-track patterns before readiness, wait for both tracks at each transition,
  and reset timing and controls per phrase. Add strict authoring and real SGB1/SGB2
  barrier, stereo, pitch, gain, stop/restart, scalar, reset and save/load checks.
  Vendor title data remains unsupported.

- Add original GBS3 two-track SPC playback with mailbox v7, independent durations,
  cursors, instruments, pan and gain. Validate both tracks before readiness and
  retain each voice's key-off state. Add strict authoring, real stereo timing,
  independent gain, finite endings, stop, scalar, reset and save/load checks on
  SGB1/SGB2. Vendor title data remains unsupported.

- Add original GBS2 uploaded-score instrument, pan and direct-gain controls,
  with mailbox v6 and full operand validation before readiness. Preserve GBS1
  limits, restore control defaults on restart, and validate real two-model stereo,
  gain, waveform selection, scalar audio, reset and save/load. Vendor title data
  remains unsupported.

- Add real SPC/DSP playback of original GBS1 uploaded scores with mailbox v5,
  complete bounded validation before adoption, one-voice notes/ties/rests and
  finite termination. Add strict authoring and actual two-model pitch, silence,
  scalar, reset, save/load and repeated-upload checks; vendor title data remains
  unsupported and legacy cold startup keeps mailbox v4.

- Relocate the original SGB resident driver to `$1000`, retaining the `$0200`
  legacy entry and reserving `$0400` with a silent unsupported-score diagnostic.
  Keep mailbox v4 and external ownership until an uploaded-score renderer exists;
  validate both-model handoff, reset, save/load and existing audio/upload behavior.

- Define the original SGB resident score layout and restart requirements, with
  a bounded offline N-SPC subset decoder and independently authored fixtures.
  Test pointer/truncation errors, unsupported commands and expansion limits;
  the current SPC firmware still does not render uploaded title scores.

- Add original SGB title-demand reporting and a bounded native firmware probe.
  Keep SOUND parameter coverage separate from post-SOU_TRN driver evidence,
  record metadata-only failure diagnostics, and document local two-model title
  runs identifying the `$2B00` score-upload / `$0400` restart compatibility gap.
  ROM-free checks cover inventory limits, real halt/capture outcomes and errors.

- Expand the original SGB diagnostic bank to five presets per effect voice with
  timer-driven rising/falling A pitch and B vibrato/tremolo. Mailbox v4 advertises
  the new effects while preserving v1/v2/v3 command limits, score uploads and
  relocated-driver adoption. Validate rendered modulation, cancellation, scalar
  audio, reset and save/load; the program prototype remains opt-in.

- Add strict JSON authoring and SOU_TRN packaging for the original SGB v3 score
  bank, with two authored example motifs. Validate actual data-only upload and
  driver rearming on SGB1/SGB2, rendered note pitches, native/combined scalar
  audio, reset and save/load; reject invalid input and existing output files.

- Add an original SGB diagnostic effect bank and two looping score motifs with
  mailbox v3. Real SPC/DSP playback supports concurrent effects/music, remembered
  preset retriggers, independent stops and global fades; retain v1/v2 contracts
  and validate scalar audio, relocation, reset and score save/load. The program
  ROM prototype remains opt-in pending broader bank and title compatibility.

- Extend the original SGB SOUND prototype with mailbox-v2 attribute staging,
  independent pitch/volume, a decaying A instrument, remembered-instrument
  retrigger and SPC-timer mute/unmute fades. Keep v1 command compatibility and
  validate rendered audio, modulation, reset and partially staged save/load.

- Add explicit mailbox-v1 adoption to the original SGB firmware prototype,
  enabling repeated SOU_TRN transfers and SOUND after compatible SPC driver
  handoff. Unknown versions remain external; arm, command and loader waits are
  bounded, with reset and save/load coverage across driver ownership changes.

- Extend the original SGB firmware prototype with exact 4 KiB ICD screen capture,
  complete-list validation, multi-block SOU_TRN upload and SPC handoff. ROM-free
  tests cover both models, upper APU RAM, invalid transfers and lifecycle state;
  arbitrary uploaded drivers have an explicit ownership boundary.

- Start the original SNES-side SGB firmware project with reproducible LoROM/SPC
  sources, two newly authored BRR instruments and ROM-free SGB1/SGB2 startup,
  restricted SOUND, reset and save/load contracts. The diagnostic prototype is
  opt-in; external SGB program ROMs remain required for production playback.

### Original replacement startup firmware

- Add bundled, independently written cold-start firmware for DMG, DMG0,
  Game Boy Pocket, CGB/CGB0, AGB/AGB0 startup-compatibility profiles, SGB and
  SGB2. Instant post-boot startup remains the default; no external boot image
  is required for these modes.
- Add original animated GBB lettering and an APU-synthesized two-note chime
  with monochrome scrolling or color fading. A/Start hides the presentation,
  not the firmware wait. SGB/SGB2 run their header bootstrap without that intro.
- Expose hardware-neutral `replacement` and `animated` startup IDs and core
  API names. Remove DMG-specific mode aliases; old setting IDs fall back to
  instant startup without migration.
- Add execution-only private-reference validation and ROM-free boot, handoff,
  reset, state, audio and serial contracts. Correct DMG startup LCD/STAT and
  serial timing and refine SGB header-transfer/peripheral handoff timing.
- Keep AGB profiles explicitly limited to GB/GBC startup compatibility on the
  CGB-E runtime baseline; this is not native GBA emulation or full AGB accuracy.

### SGB firmware host correctness

- Bundle an independently written 64-byte SPC700 IPL with reproducible source
  under `firmware/spc700/`. Desktop firmware playback now needs only the selected
  SGB1/SGB2 program ROM; `spc700.rom` remains an optional validated override.
  Add opaque-reference upload/timing, reset and restore contracts and exact
  native/combined/scalar and production-adapter playback gates.
- Extend bundled SGB1/SGB2 desktop adapter regression coverage through boot
  handoff, stereo 48 kHz audio, viewport output, reset and snapshot restoration.
  Add an optional private-title production replay and a Windows playback mode
  that excludes external GB boot overrides using isolated firmware copies.
- Correct complete cold reset and restore destination-owned audio callbacks.
- Clock raw ICD LCD pixel and physical-line availability at PPU emission
  boundaries instead of publishing whole rows at the next scanline. Preserve
  initialized ring RAM on reuse/LCD restart and queue instruction-tail output
  until its host rendezvous. Concurrent producer reads remain an explicitly
  unqualified deterministic approximation.
- Add LCD timing observations, transfer-fault context and boundary contracts.
  Experimental whole-host snapshots advance to `GBBSHOST` version 3; previous
  host containers are rejected, while ordinary GB save-state format remains
  separate.
- Validate exact native/restored/scalar playback and production PCM-prefix
  parity. Record failed clocked-bridge headroom measurements without relaxing
  thresholds or reducing output quality; performance qualification remains open.
- Cache the earliest deferred LCD/CPU rendezvous to avoid redundant GB clock
  conversion. Preserve complete audio/state pins and add dense-master-clock
  LCD equivalence checks; the measured four-profile p05 headroom gate still fails.

## [0.36.0] - 2026-10-04

### Experimental Super Game Boy firmware playback

- Add an opt-in desktop SGB1/SGB2 firmware host with independently implemented
  SNES CPU, ICD/Game Boy bridge, SPC700, and DSP components, plus combined
  Game Boy and SNES-side audio through the existing SDL playback path.
- Expose firmware playback settings in the Windows and Linux library dashboards,
  with model/directory validation and explicit command-line overrides. Changes
  apply to the next ROM launch; reset retains the running backend and model.
- Support pause/resume, cold reset, and whole-host manual snapshots. Keep
  firmware battery saves and quick states separate from ordinary HLE saves,
  with firmware/model-bound state validation and staged save replacement.
- Optimize exact host execution, audio mixing, and replay caches without
  changing validated output. Add headroom gates, benchmark provenance, and
  native Windows playback/audio qualification tools.
- HLE remains the default on every frontend. Experimental firmware playback
  requires legally obtained user-supplied images; no firmware or external
  emulator core is bundled. Android and Web do not expose this playback path.
- Retain the existing SGB color/border compositor: full SNES graphics/menu
  rendering is not implemented. Voxel presentation, link sessions, debugger,
  cheats, automatic rewind, and unsupported cartridge peripherals remain
  unavailable in the experimental firmware backend.

### SGB accuracy and diagnostics

- Add deterministic title/gameplay replay and independent frame/audio comparison
  tools, with explicit input, clock, boot, upload, and scene provenance.
- Correct SGB LCD restart frames and diagnostic cold-boot/input replay timing.
  Expand host startup, PPU/DMA, APU upload, port rendezvous, and shared-bus traces.
- Add ROM-free SPC700/DSP timing and PCM contracts covering BRR decoding,
  interpolation, envelopes, key polling, noise, pitch modulation, echo/FIR,
  register writes, and resumable cycle execution.
- Preserve unresolved boot/fade/handshake and title-reference mismatches as
  documented limitations; these tools do not establish complete SGB accuracy.

### Desktop, CI, and documentation

- Fix Windows dashboard control-ID collisions, unsupported native-menu actions,
  settings bounds, and modal completion. Exercise clipped scrolling controls
  and short-window layouts in the native dashboard smoke test.
- Stabilize TCP diagnostic replay and make SGB trace contracts portable across
  platforms, including Windows line endings, compiler setup, and stack usage.
- Correct CI regression-report artifact paths and update GitHub Pages actions.
- Add Android and voxel screenshots to the README and document experimental
  firmware setup, backend restrictions, diagnostics, and measured qualification.

## [0.35.25] - 2026-09-25

### Super Game Boy

- Correct SGB border layering in the SDL frontend and add SGB visual and
  performance validation gates.
- Pace NTSC SGB1 at its faster Game Boy clock while retaining the normal SGB2
  clock, including model-specific audio resampling.
- Keep independent input states for up to four SGB controllers, with extra
  gamepads and a second keyboard layout available through SDL. Preserve those
  states in save-state format version 37.
- Add an SGB trace command inventory to identify missing SNES-side features;
  SNES audio, sprite, and host-control commands are not yet emulated.

### Audio accuracy

- Align APU length-counter clocking with the skipped startup divider edge.

## [0.35.24] - 2026-09-23

### Voxel diorama rendering

- Preserve native source-pixel detail in all desktop and web diorama modes,
  including the regular and shape-aware renderers.
- Batch the recessed framebuffer into a textured plane and generate 3D geometry
  only for columns with actual relief, reducing the per-frame geometry cost
  without introducing the visual artifacts caused by coarse 2×2 cells.

## [0.35.23] - 2026-09-23

### Super Game Boy diagnostics

- Add Android SGB trace capture and replay support, including a trace
  comparison tool for investigating hardware-specific behavior.
- Normalize trace boundary checkpoints and accept CRLF-delimited traces so
  captures remain comparable across platforms.
- Add deterministic SGB trace regression coverage.

### Link cable testing

- Make the replayable link fault harness wait for a valid host-led handshake
  and drain pending injected packets before declaring a scenario settled,
  preventing timing-sensitive failures on slower runners.

## [0.35.22] - 2026-09-23

### Link cable

- Preserve the outgoing serial byte after a remote link packet is queued,
  preventing Pokémon's probe-byte rewrites from corrupting TCP link handshakes.
- Add regression coverage for delayed remote responses before the first serial
  edge.

## [0.35.21] - 2026-09-22

### Link diagnostics and reliability

- Add structured link diagnostics, persisted diagnostic bundles, and a
  replayable TCP fault harness for drop, delay, duplicate, and disconnect
  scenarios.
- Exercise the real link harness across desktop platforms and retain failure
  traces and artifacts for investigation.

### Desktop UI and packaging

- Stabilize Windows dashboard repaint transitions and settings scrolling.
- Package Linux desktop releases as self-contained AppImages.
- Add release asset preflight validation for Linux, Windows, macOS, Android,
  and Web artifacts.

### Testing and release automation

- Expand accuracy, sanitizer, ABI, fuzz, hardware-model, and platform smoke
  coverage.
- Add a manually runnable release preflight workflow that validates artifacts
  without publishing a release.

## [0.34.2] - 2026-09-13

### Android

- Keep one-finger secondary A/B actions held continuously while preserving the
  primary action, allowing repeated run-and-jump input without short clicks.
- Synchronize touch hit testing with the branded control layout and add motion
  grace for stable action-button handoffs.
- Avoid automatic rewind snapshot serialization on Android and optimize native
  debug builds so on-device gameplay stays within the frame budget.

## [0.34.1] - 2026-09-12

### Super Game Boy

- Show a black border area while a cartridge's custom SGB border is loading,
  keeping the branded fallback visible only when no custom border transfer has
  started.
- Refine the default Super Go Bigger Boy border artwork with stepped shell
  edges, indicator lamp, framing, and logo details.
- Persist the SGB border-loading state in save-state format version 29.

## [0.34.0] - 2026-09-11

### Super Game Boy

- Add a branded default SGB/SGB2 border for games that do not upload a custom
  border, including the `SUPER` and `GO BIGGER BOY` wordmark.
- Preserve cartridge-provided borders when a game supplies custom border data.

### Android controls

- Allow one finger to hold B while activating A, then release A while keeping
  B held for platform-game run-and-jump controls.

## [0.33.18] - 2026-09-11

### Android controls

- Repair portrait and landscape touch controls to match the approved branded
  layouts, with stable sizing, crisp layered buttons, readable labels, and
  the dark navy and cyan visual treatment.

## [0.33.17] - 2026-09-11

### Android emulation

- Resume the already-running ROM when selecting it again from the library
  instead of restarting the emulation session.
- Add branded portrait and landscape touch controls with labeled buttons,
  expanded gameplay viewports, and consistent Go Bigger Boy styling.

## [0.33.16] - 2026-09-10

### Windows settings

- Show the hardware-model restart notice immediately after a confirmed model
  selection instead of delaying it until the next ROM launch.

### Super Game Boy

- Render a deterministic SGB BIOS-style fallback border before a cartridge's
  custom `PCT_TRN` data arrives, restoring visible borders for games that defer
  or omit custom border uploads on all frontends.
- Arm the SGB command receiver at adapter startup so the first `00` start pulse
  is accepted; this restores Pokémon and other games' initial `MLT_REQ`,
  `CHR_TRN`, and `PCT_TRN` border setup sequence.
- Correct SGB screen-data transfer timing to the documented five-frame window,
  preventing border graphics from being sampled before a game has finished
  preparing the transfer image.
- Make an explicit SGB/SGB2 model selection override a cartridge's CGB
  compatibility flag, allowing dual-mode games to enter their SGB border path.

## [0.33.15] - 2026-09-10

### Link diagnostics and audio continuity

- Record serial bit-progress boundaries and elapsed/frame deltas for Pokémon
  link state transitions, making guest-side waits distinguishable from cable
  scheduling delays.
- Keep a short silence cushion on the SDL audio stream while an active link
  session is waiting for the peer, avoiding repeated audio and underruns.
- Extend trace parsing and reporting with serial-progress and transition
  counters, with regression coverage for the new event fields.

### Super Game Boy

- Decode sampled `CHR_TRN`/`PCT_TRN` data into a deterministic 256×224 SGB
  border framebuffer, including SNES 4bpp tiles, tilemap flips, RGB555 border
  palettes, and transparent overlay of the native Game Boy viewport.
- Expose the SGB framebuffer through the core, emulator, and memory-bus APIs.
- Route SGB/SGB2 frames through the frontend-neutral video contract so SDL,
  Android, and web presentation use the 256×224 dimensions automatically.
- Persist the PCT transfer latch in save-state format version 27 and add
  compositor regression coverage.
- Model the hardware's three-frame latency for `CHR_TRN`, `PCT_TRN`, `PAL_TRN`,
  and `ATTR_TRN`, including in-flight transfer state in save-state version 28.

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
