# Architecture and maintainability audit

Date: 2026-09-04

## Summary

The project has a sound first abstraction in `gbb::EmulatorCore`, and the
ordinary SDL, Android, Web, and CLI paths now use it. Advanced development
tools and some link/UI paths still depend on the Game Boy adapter, while a
stable dynamic-plugin ABI is intentionally not promised yet. The emulator core
is testable and well covered, and the remaining organization work is focused on
making those optional boundaries and diagnostics just as explicit.

The recommended approach is incremental extraction, not a rewrite.

## Highest-priority findings

| Area | Evidence | Risk |
| --- | --- | --- |
| SDL frontend | `apps/sdl/main.cpp` was 9,118 lines at audit time and is now about 1,899 lines after incremental extractions; resource construction/teardown, desktop storage/dialogs, dashboard navigation, shared event routing/tool controls, event-policy ordering, voxel, linked-frame presentation, Windows menu, camera capture, audio output, all desktop tool windows, Android touch input, binding configuration, lifecycle cleanup, ROM/link session lifecycle, frame pacing, and the Android JNI bridge are isolated | Very high search and regression cost |
| Android frontend | `LibraryActivity.java` was 1,006 lines and combined library, settings, artwork, touch-layout editing, and update UI; library and settings screens, touch-layout editing, artwork, and updates are now isolated and the activity is about 300 lines | UI changes can affect unrelated flows |
| Link harness | `apps/link_harness/main.cpp` was 2,008 lines at audit time and is now about 440 lines; scenario state, Pokémon probes/automation, trace formatting, trace-file lifecycle, and semantic detection are isolated | Difficult to isolate trade/battle failures |
| Tests | `tests/core_tests.cpp` is now about 314 lines with manually invoked smoke tests in `main()`; 38 subsystem and integration contracts are registered as separate CTest executables | Deeper malformed-input and replay coverage remains |
| Save states | `src/save_state.cpp` remains the hardware-field codec for CPU, bus, PPU, APU, cartridge, camera, and SGB state (now about 926 lines); container framing, ROM identity, size limits, and CRC validation are isolated in `src/save_state_container.cpp` | Every new field increases compatibility risk |
| PPU/cartridge | `src/ppu.cpp` now owns register/memory behavior while `src/ppu_timing.cpp` owns scanline timing, fetcher, sprite, and pixel composition; cartridge mapping is separated from persistence/peripheral storage in `src/cartridge_persistence.cpp` | Hardware bugs are hard to localize |

## Core and frontend architecture

### What is working

- `gbb::EmulatorCore` provides descriptors, capabilities, input, video, audio,
  persistence, and save-state operations.
- `CoreRegistry` supports ROM probes and factory registration.
- Scene snapshots and JSON export provide a useful presentation/debugging seam.
- Link transport framing is separated from the local serial implementation.

### Boundaries that are not yet enforced

The SDL frontend directly includes and uses `gameboy::Emulator` throughout
`apps/sdl/main.cpp`; Android uses the same translation unit. The Web and CLI
frontends now use generic printer/metadata/debugger APIs for their ordinary
paths, but SDL still relies on concrete Game Boy services for its advanced
debugger, link, camera, and editing tools.

A future GBA core could be registered, but it would not work through the mature
SDL/Android feature paths without adding more Game Boy-specific conditionals.

`gameboy::LinkSession` now consumes the core-neutral `gameboy::LinkEndpoint`
contract rather than a concrete `gameboy::Emulator&`. The built-in Game Boy
core is connected through `GameBoyLinkEndpoint`; future cores can provide the
same serial-port/step adapter without changing the scheduler or transport.

The public registry is extensible in-process: `built_in_core_factories()` owns
the current Game Boy contribution while `CoreRegistry` owns validation and
selection policy. The build still contains only one built-in core, so adding a
second core requires its own target and one contributor entry; no probe or
frontend-selection logic needs to change. Probe ties are still not part of the
return value, but equal-confidence selections now emit a warning naming the
candidates and deterministic winner.

`SceneSnapshot` retains Game Boy-shaped compatibility fields such as LCDC,
SCX/SCY, WX/WY, CGB palettes, and OAM coordinates. Scene population is now
centralized in `src/gameboy_scene.cpp`, and the schema has an explicit version
plus optional opaque, core-defined layers. Future work should use those layers
and add renderer capability contracts instead of adding more hardware-shaped
members.

`gbb_core_api` is described as system-neutral, but it links directly to
`gameboy_core`. This is acceptable for the current static build, but it is not
yet a stable plugin ABI.

## Logging and diagnostics

Diagnostics are useful but fragmented:

- PPU window tracing is bootstrapped by `GBB_TRACE_WX` and now routes through
  the shared logger level, so debuggers and tests can enable or disable trace
  records at runtime without a static file handle.
- SDL link tracing retains frontend-specific payload formatting, while
  file/session lifecycle is centralized in `LinkTraceFile`.
- The link harness retains scenario-specific payloads, while trace-file
  lifecycle and buffering now live in a reusable writer.
- Core frontend errors and warnings now pass through the shared logger before
  retaining their platform-specific presentation; harness/test output and
  intentional command-line usage text still use direct stderr.
- Link-harness and SDL trace events now share a canonical versioned prefix
  (`event`, `trace_version`, `session_id`, `frame`, `elapsed_ms`, `transport`,
  and `role`); payload fields remain frontend-specific for now.
- Synchronous event flushing is bounded, but some frontend paths can still
  perturb timing during link debugging.

SDL, CLI, Web, and the link harness now establish presentation frame, ROM
identity, and (where available) emulated-cycle context through
`gbb::LogContextScope`; link sessions provide their session and endpoint-cycle
context. User-facing CLI reports and usage errors intentionally remain direct
output, while runtime diagnostics use the platform-neutral logger. Writes
should remain buffered, with an explicit flush at session end and a bounded
ring buffer for failure context.

SDL file-dialog callbacks and Android JNI back/ROM requests now capture the
initiating context and restore it at the asynchronous handoff boundary. Their
completion, cancellation, and failure records therefore retain the frame and
ROM identity that caused the request, even when the callback runs on another
thread. Desktop update-check and download workers use the same handoff
contract when their results return to the SDL loop.

## Test architecture

Current coverage is strong and subsystem contracts are registered as separate
CTest targets. The remaining test-architecture work is deeper input validation
and failure reproduction rather than another broad split of the core smoke
executable.

Still open:

There are no mandatory implementation items for the current static
architecture. The reviewed corpus has a checksum manifest, both fuzz
workflows enforce deterministic hygiene, and the TSan job covers the complete
SDL-enabled contract suite plus a bounded Xvfb dashboard launch.

Deferred follow-up:

- future generated fuzz inputs still require human semantic review before
  promotion;
- longer user-driven GUI TSan sessions require a real display and input
  device, beyond the automated dashboard smoke;
- a dynamically loaded core-plugin ABI should wait until the static API has
  stabilized.

## Concrete issue found during the audit

Version metadata originally had multiple sources. `CMakeLists.txt`,
`android/app/build.gradle`, and the web entry point each carried independent
defaults, which could produce updater and displayed-version inconsistencies.
The repository `VERSION` file is now the source for CMake and Android, while
desktop and web targets receive `GBB_VERSION` from CMake (with development
fallbacks for standalone source builds).

## Recommended implementation order

1. Establish guardrails: unify versioning, add dependency checks, and add
   architecture contract tests. **Complete.**
2. Introduce shared logging and trace infrastructure. **Complete for the
   current static frontends, including asynchronous callback propagation.**
3. Remove duplicated scene extraction and centralize settings parsing.
   **Complete.**
4. Extract shared presentation/video transforms. **Complete.**
5. Move SDL, web, and Android normal execution to `EmulatorCore`.
   **Complete for ordinary execution; advanced tools remain adapter-specific.**
6. Generalize link sessions around a core-independent endpoint. **Complete.**
7. Split the SDL frontend, link harness, Android activity, save-state codec,
   PPU, and cartridge implementation. **Largely complete; optional-tool
   lifecycle and capability isolation is now centralized at the SDL dispatch
   boundary.**
8. Consider dynamically loaded core plugins only after the static API is stable.

The static audit implementation is complete. Future work should follow the
deferred items above rather than introducing a dynamic plugin ABI before the
core contract has settled.

## Progress

The first guardrail pass is implemented:

- `VERSION` is now the local source for CMake and Android version metadata;
  tagged CI builds can still override it from `GITHUB_REF_NAME`.
- Game Boy scene extraction now has one implementation instead of duplicate
  copies in the registry adapter and scene helper.
- `gbb::Logger` provides level/category filtering, optional file output,
  session/frame/cycle context, and an opt-in bounded in-memory sink for tests
  and failure reports. The PPU window trace uses it while retaining the
  opt-in `GBB_TRACE_WX` switch.
- Shared palette, LCD, and smoothing transforms now live in `gbb::video` and
  are used by both SDL and Web presentation paths.
- Core registry validation has regression coverage for invalid and duplicate
  factories, including factories supplied through the constructor. Each
  registry can now expose per-core probe matches for diagnostics, and core
  creation logs the candidates and selected core through the shared logger.
- Core contract validation now checks scene capability invariants as well as
  framebuffer/audio/input metadata: advertised scene snapshots must have a
  version, match the core's video dimensions, and use named, bounded opaque
  layers. Invalid adapters fail at creation with an actionable error.
- Scene snapshots now identify their producer adapter, reject duplicate opaque
  layer IDs, and require payloads for sized layers. This keeps future core
  extensions namespaced and prevents ambiguous or silently empty render data.
- The common `settings.ini` key/value reader now lives in `gbb::read_settings_file`;
  SDL's schema-specific settings code consumes it, and the native Android/SDL
  entry points no longer need independent comment/whitespace parsing.
- Logger consumers can opt into a bounded recent-record buffer, making it
  possible to attach the last diagnostic events to a report without requiring
  a writable log directory.
- `gbb::LogContextScope` now provides nested, thread-local session/frame/cycle
  and ROM context inheritance. Frontend and transport code can establish
  context once at a boundary, while a helper may override only the field it
  owns; the logging contract test covers nesting, restoration, and hexadecimal
  ROM formatting. SDL establishes the current presentation frame and active
  ROM fingerprint at the runtime-loop boundary, so diagnostics emitted by
  event, link, audio, and presentation helpers are tagged consistently.
- CLI, Web, and link-harness execution boundaries now establish the same
  session/frame/cycle/ROM context, so non-SDL diagnostics can be correlated
  with the operation that produced them.
- Core-neutral link endpoints now expose an optional cycle position for
  diagnostics without making scheduling depend on Game Boy CPU types. The
  built-in adapter reports its CPU cycle counter, while third-party endpoints
  may retain the zero default.
- SDL input, shortcut, and touch settings models/constants now live in
  `apps/sdl/settings_model.hpp`, reducing the amount of configuration state
  declared in `main.cpp` while preserving the existing file format and UI
  behavior.
- Legacy `controls.txt` and `palette.txt` migration is isolated in
  `apps/sdl/settings_persistence.cpp`; the main SDL translation unit now only
  consumes those migration helpers.
- Settings model defaults, platform path resolution, and scalar value parsing
  now also live in the settings persistence module; the schema loop is now
  consumed through the same persistence contract.
- Portable settings serialization, completion of missing entries, and SDL
  key/gamepad name conversion now live in `apps/sdl/settings_persistence.cpp`;
  `main.cpp` retains only the runtime-facing load/save calls.
- The complete SDL settings loader and load/save wrappers now live in
  `apps/sdl/settings_persistence.cpp`; the Android JNI and desktop UI callers
  use the same module contract, reducing `main.cpp` by roughly 500 lines.
- Bounded frame stepping now lives in `gbb::advance_to_frame`, keeping the
  execution policy independent from Web/SDL presentation code. The helper
  reports the actual cycles executed (including an indivisible instruction
  crossing the budget) and deliberately leaves frame consumption to the
  frontend.
- Web uses the generic `EmulatorCore` overload, while SDL's ordinary
  single-console loop uses an explicitly marked transitional Game Boy overload.
  Link/debugger/replay paths remain on their existing per-instruction loops;
  this avoids pretending those concrete-only capabilities are generic before
  their adapters exist.
- Local `LinkSession` start, retry, stop, and timeout transitions now emit
  structured logger records. This gives link failures a stable lifecycle trail
  even when the optional SDL trace file cannot be created.
- TCP serial endpoints now use the same logger for attach/detach, hello
  handshake, denial/not-ready backoff, and unmatched-response events. Existing
  counters and packet behavior are unchanged; the records add causal context
  for intermittent Cable Club failures.
- Web palette presentation now uses the generic core descriptor to identify
  CGB output instead of down-casting during ordinary rendering. The remaining
  Web Game Boy adapter use is limited to enabling the optional printer
  peripheral during ROM setup.
- Completed printer pages now cross the `EmulatorCore` boundary as generic
  `PrinterPage` values. Web no longer reaches into the Game Boy bus to drain
  printer output; bitmap encoding remains a separate presentation concern.
- Printer endpoint activation now also crosses `EmulatorCore` through
  `set_printer_enabled`, removing Web's remaining concrete-emulator cast from
  ROM setup while retaining capability gating.
- Core descriptors now expose optional software/cartridge metadata, and an
  optional program-counter accessor keeps CLI diagnostics generic. The CLI no
  longer down-casts merely to print ROM information or PC state.
- A reusable `validate_core_contract` guard now checks descriptor, input, and
  framebuffer invariants before frontend media setup. CLI and Web reject an
  invalid adapter with a clear error, and a dedicated contract test instantiates
  every built-in factory so adding a core cannot silently break the boundary.
- SDL's primary session now owns `gbb::EmulatorCore`; the concrete Game Boy
  pointer is a non-owning adapter reserved for link, debugger, camera, and
  editing tools. Ordinary ROM loading, timing/pacing, frame stepping,
  framebuffer sizing/presentation, audio, touch/gamepad/keyboard input, quick
  saves, rumble, and printer output use the generic contract. Core-provided
  dimensions and clock cadence are applied when a ROM is loaded.
- Registry creation now rejects a factory that returns a null core and logs the
  failing core ID, preventing an invalid plugin/core from surfacing later as a
  frontend null dereference.
- SDL's remaining Game Boy-only desktop tools now enforce the capability mask
  at every entry point: Windows menu enablement, keyboard/menu dispatch,
  debugger/TAS/cheat/sprite processing, camera capture, voxel mouse orbit, and
  link-session requests. A future core can therefore run the generic SDL loop
  without accidentally invoking a null or semantically incompatible adapter.
- The capability policy is centralized in `apps/sdl/core_capability.hpp` and
  covered by a standalone SDL capability contract test, keeping this rule
  testable without opening a window or depending on a specific Game Boy ROM.
- `GameBoyToolAdapter` now combines the non-owning Game Boy adapter pointer
  with the owning core's capability mask. Desktop tool-event and voxel-camera
  dispatch use this view, so optional tool lifetimes and capability checks are
  enforced at one boundary instead of being independently repeated.
- Core selection is now separated from built-in contributions: the registry
  consumes `built_in_core_factories()`, while the Game Boy adapter owns its
  factory and implementation in `src/gameboy_core_factory.cpp`. Adding another
  statically linked core no longer requires editing probing or selection code.
- `SceneSnapshot` now has an explicit schema version and optional opaque
  `SceneLayer` extensions (`id`, format, dimensions, payload). Existing Game
  Boy fields and voxel behavior remain compatible, while future cores can add
  namespaced scene data without introducing new Game Boy-shaped members.
- TCP endpoint diagnostic records now carry a per-attachment session ID, so
  reconnects and retries can be correlated with local-link lifecycle records.
- Logger file sinks now create missing parent directories, and the regression
  suite covers nested temporary paths so diagnostics do not disappear merely
  because a configured directory has not been created yet.
- Cycle-balanced link scheduling now lives in the core-neutral
  `gbb::advance_balanced` callback primitive. `LinkSession` consumes the
  core-neutral `LinkEndpoint` contract, leaving the scheduler reusable by
  future cores without importing Game Boy types.
- `LinkSession` now depends only on the core-neutral `LinkEndpoint` interface.
  `GameBoyLinkEndpoint` contains the built-in emulator adapter, SDL and the
  link harness keep those adapters alive alongside their sessions, and the
  serial-link contract suite verifies a non-Game-Boy-style endpoint can be
  scheduled. This makes adding another core a local adapter change rather than
  a change to link-session internals.
- SDL link trace file/session lifecycle now lives in
  `apps/sdl/link_trace_file.cpp`; the large frontend keeps only guest-specific
  field formatting. Temporary-directory fallback, session IDs, elapsed time,
  and flush/close behavior are centralized without changing the trace schema.
- SDL and link-harness diagnostic events now use the same canonical event
  prefix from `gbb/trace_format.hpp`, allowing one parser to correlate frame,
  serial, Pokémon-state, and harness watchdog records while preserving their
  domain-specific payload fields.
- `gbb::parse_trace` now validates that canonical envelope, accepts legacy
  `frame=` records, and produces a typed summary of session context, frame
  ordering, serial completions, Pokémon transitions, trade phases, and stall
  markers. Replay-style contract tests cover representative SDL and harness
  traces plus malformed records.
- Trace parsing now has explicit byte, line, record, field, and error limits;
  deterministic mutation and oversized-input tests exercise those bounds.
  Linux CI runs the full native contract suite with ASan, UBSan, and leak
  detection enabled.
- Settings parsing, link-packet framing, and SGB command handling now include
  deterministic mutation/property matrices. They exercise generated valid
  round trips, malformed lines, truncated command packets, all command IDs,
  and single-bit wire corruption without introducing nondeterminism into the
  normal CTest run. Save-state container mutation coverage already provides
  the equivalent checksum, truncation, size, and header checks.
- Link packet framing is now implemented in `src/link_packet_codec.cpp`,
  separate from the in-process cable transport. This keeps wire validation and
  checksumming reusable by future network backends and independently
  discoverable in coverage.
- TCP channels now count rejected complete frames per connection and include
  that count in SDL remote traces, making malformed wire data distinguishable
  from a stalled guest handshake.
- Pokémon Cable Club title/UI probes now live in
  `apps/sdl/pokemon_link_diagnostics.cpp`; trace formatting can consume the
  guest-state detector without embedding VRAM scanning and localization
  details in the SDL event loop.
- Link-harness command-line parsing and scenario validation now live in
  `apps/link_harness/options.cpp`, leaving the harness entry point focused on
  emulation and outcome reporting.
- SDL event-loop state is now represented by a single `SdlEventContext`
  aggregate. The dispatcher retains its existing event ordering and behavior,
  but its 30-plus parameter interface is gone; this provides a stable seam for
  extracting dashboard, input, link, and platform-specific handlers without
  repeatedly editing the main runtime loop.
- Dashboard keyboard navigation is now a focused event handler behind that
  context seam; dashboard behavior can be tested and extended independently
  from gameplay input and platform event handling.
- Keyboard-binding capture/validation is likewise isolated from gameplay
  shortcuts, preserving the existing duplicate/reserved-key rules while
  reducing the dispatcher's platform-agnostic branch surface.
- Gamepad dispatch now has the same focused boundary, covering dashboard
  navigation, rebinding, and gameplay input while preserving desktop and
  Android input-movie differences.
- The keyboard shortcut/gameplay chain now lives in a dedicated handler. Its
  precedence (speed controls, state actions, link controls, pause/fullscreen,
  exit, then gameplay input) is explicit and no longer expands the central
  event switch.
- Mouse dashboard interaction is isolated from keyboard/gamepad dispatch,
  including coordinate conversion, scrolling, menu-button activation, and
  desktop voxel camera handling remains in its existing pre-dispatch path.
- Android touch gestures and lifecycle transitions now have dedicated
  handlers. Touch ownership/orbit behavior, background save flushing, and
  foreground settings refresh no longer share the central switch body.
- Focus-loss cleanup and gamepad attach/detach are isolated as lifecycle/device
  handlers, keeping input release and rumble reset behavior together.
- SDL event pumping now lives in `apps/sdl/event_dispatch.cpp`, while
  application event-policy ordering lives in `apps/sdl/event_policy.cpp`.
  `main.cpp` supplies one context and platform callbacks, so SDL queue
  ownership and policy can be replaced or instrumented independently in tests.
- SDL event-policy ordering now lives in `apps/sdl/event_policy.cpp`; the
  runtime loop supplies one context and platform-specific callbacks, keeping
  shutdown prompts, Android back handling, dashboard precedence, and event
  dispatch ordering out of `main.cpp`. The Android leave-game callback now
  receives the owning core and non-owning adapter explicitly, preventing an
  invalid ownership/signature seam from being hidden in the main loop.
- The event pump has a headless contract test covering shutdown short-circuiting
  and delivery of queued SDL user events.
- `SdlEventContext` now lives in `apps/sdl/event_dispatch.hpp` alongside the
  event-pump contract, so the context is no longer an implementation detail of
  `main.cpp` and can be shared by the remaining policy handlers.
- Gamepad device attach/detach policy now lives in `event_dispatch.cpp`, with
  SDL resource ownership changes covered by the same dispatcher boundary.
- Focus/background/foreground cleanup now lives in `apps/sdl/event_lifecycle.cpp`,
  keeping save flushing, touch refresh, input release, and rumble reset out of
  the main translation unit while retaining the dispatcher API.
- Dashboard mouse policy now consumes callback-based row lookup, selection
  activation, and library-open actions from `SdlEventContext`; coordinate/UI
  behavior is therefore decoupled from `main.cpp`'s dialog implementation.
- The context now exposes callback seams for dashboard item lookup, row hit
  testing, activation, and library opening. This keeps the dispatcher module
  independent of dashboard dialogs and platform window-management code.
- Android touch dispatch now uses the same callback seams from the dispatcher
  module; the legacy in-file wrapper has been removed.
- Dashboard keyboard navigation now uses the dispatcher module and injected
  help, ROM-dialog, activation, and exit-confirmation callbacks, keeping the
  dashboard policy independent from `main.cpp` UI helpers.
- Keyboard rebinding and gameplay shortcut handling now use the same dispatcher
  boundary. Save/load state, link controls, pause/fullscreen, palette selection,
  and ordinary button translation are expressed through `SdlEventContext`
  state and callbacks instead of a second private event-policy implementation
  in `main.cpp`.
- Windows menu commands now use a dedicated dispatcher handler as well. Palette
  and video changes, save/load, link controls, tools, and exit actions no
  longer occupy the main event-loop body; platform UI callbacks remain injected
  through `SdlEventContext`.
- Desktop tool-window event ownership and voxel-camera mouse gestures now have
  dedicated dispatcher handlers. The main loop only applies their ordering
  policy, while the camera-mode predicate has a small direct regression test.
- Frontend diagnostics now share `gbb::log_frontend_*` helpers. SDL dialogs,
  audio/camera/input warnings, settings and link-trace notices, CLI failures,
  and Web SDL failures all reach the structured frontend logger without
  changing their user-facing behavior.
- SDL and link-harness traces now share a versioned session lifecycle schema:
  process-wide session IDs, transport/role/scenario metadata, monotonic timing,
  and session-end records. Both writers checkpoint buffered output every 30
  emulated frames, while serial-stall and shutdown paths still flush eagerly.
- Pokémon-specific WRAM bank guards, party/menu probes, Cable Club validation,
  handshake reset, and map/input helpers now live in
  `apps/link_harness/pokemon_state.*`; the harness orchestration no longer owns
  those hard-coded guest addresses.
- Scenario trace record formatting and serial-progress watchdog state now live
  in `apps/link_harness/scenario_trace.*`; the entry point only schedules
  frames and reports outcomes while the trace module owns diagnostic schema
  details.
- The deterministic Pokémon input state machine now lives in
  `apps/link_harness/pokemon_automation.*`; Cable Club navigation and trade /
  battle menu timing no longer enlarge the harness entry point.
- The extracted harness parser now has its own CTest executable in
  `tests/link_harness_options_tests.cpp`, covering valid scenarios and the
  required-argument, paired-state, numeric, and transport validation paths.
- Harness trace lifecycle now has its own `ScenarioTraceWriter` module and
  focused `tests/link_harness_trace_tests.cpp` target. Header/termination
  records, periodic frame checkpoints, and explicit event flushes are tested
  independently from command-line parsing.
- Link wire framing now has its own CTest executable in
  `tests/link_packet_codec_tests.cpp`, covering round trips, truncation,
  checksums, null buffers, and unknown packet types independently of the
  larger emulator test binary.
- CMake now exposes an opt-in `GAMEBOY_ENABLE_SANITIZERS` configuration for
  native ASan/UBSan runs (ASan on MSVC), keeping sanitizer checks available for
  CI and debugging without changing release-build behavior. Example:
  `cmake -S . -B build-sanitize -DGAMEBOY_ENABLE_SANITIZERS=ON` followed by
  `cmake --build build-sanitize` and `ctest --test-dir build-sanitize`.
  In ptrace-restricted environments, LeakSanitizer can abort before a test
  starts; use `ASAN_OPTIONS=detect_leaks=0 LSAN_OPTIONS=detect_leaks=0` for
  ASan/UBSan execution and run leak checks outside that restriction.
- CMake also exposes `GAMEBOY_ENABLE_THREAD_SANITIZER` for native GCC/Clang
  builds. It is intentionally separate from the address/undefined sanitizer
  option because the runtimes cannot be combined; unsupported MSVC and mobile
  configurations fail during configure with an actionable message.
- `GAMEBOY_BUILD_FUZZERS` now builds a Clang/libFuzzer target that feeds one
  bounded input to the settings, trace, save-state, link-packet, and SGB
  parsers. It is opt-in so ordinary builds and CTest remain dependency-free;
  combine it with sanitizer flags for local campaigns and persist promising
  inputs as a future corpus. A small reviewed seed corpus lives under
  `tests/fuzz/corpus`, and CI runs a bounded 45-second smoke campaign while
  uploading crash artifacts for follow-up. A separate weekly/manual workflow
  runs a thirty-minute campaign and uploads the evolved corpus for review.
  `tests/fuzz/run_campaign.sh` is the same bounded runner for local campaigns;
  it stages reviewed seeds into a separate directory and treats only the
  expected timeout as successful, so crashes remain actionable failures.
  `tests/fuzz/promote_corpus.sh` minimizes a reviewed campaign in a temporary
  directory and requires `--approve` before copying any new inputs into the
  checked-in seed corpus.
- Core frame stepping and balanced link scheduling now have a dedicated
  `tests/core_runtime_link_scheduler_tests.cpp` executable; those contracts no
  longer contribute to the monolithic core test binary.
- Shared settings parsing and video presentation transforms now have a
  dedicated `tests/settings_video_contract_tests.cpp` executable, separating
  frontend-facing contract failures from hardware-core regressions.
- Full emulator save-state round-trip, validation, corruption rejection,
  legacy-version compatibility, hardware pipeline restoration, and RTC state
  coverage now run in `tests/save_state_contract_tests.cpp`. The core smoke
  suite no longer carries the save-state codec's large compatibility matrix.
- CGB speed switching, banked VRAM/WRAM, palettes, DMA, compatibility mode,
  debugger register edits, and CGB save-state coverage now run in
  `tests/cgb_contract_tests.cpp`, isolating color-hardware regressions from
  cartridge persistence and the remaining smoke checks.
- PPU scanline timing, mode transitions, background fetcher, window handoff,
  sprite scheduling, pixel composition, and trace emission now live in
  `src/ppu_timing.cpp`; `src/ppu.cpp` retains construction and register/memory
  access behavior. This keeps timing changes reviewable without mixing them
  with the register map.
- Cartridge persistence paths are now isolated in
  `src/cartridge_persistence.cpp`: battery save import/export and flushing,
  RTC serialization/latching, and persistence-path loading no longer share a
  translation unit with mapper reads/writes and camera image processing.
- The SDL voxel diorama renderer now lives in `apps/sdl/voxel_renderer.cpp`
  behind a small `VoxelRenderContext` instead of depending on the monolithic
  `SdlResources` class. Geometry generation, profile loading, camera offsets,
  and facade presentation can now be changed and tested independently from
  the SDL event loop; the renderer reports presentation failure to its caller
  so frontend error policy remains centralized.
- Linked-frame conversion and local/TCP status overlays now live in
  `apps/sdl/frame_presenter.cpp` behind `FrameRenderContext`. The main loop
  only assembles presentation state and applies the frontend's error policy;
  video conversion and link diagnostics can evolve independently.
- The Windows desktop menu bar now lives in
  `apps/sdl/windows_menu_bar.cpp` behind a small ownership wrapper. Native
  Win32 menu construction and command interception no longer expand the SDL
  event-loop translation unit, while command IDs and enable/check state remain
  unchanged.
- Game Boy Camera device ownership, permission handling, orientation/cropping,
  and grayscale conversion now live in `apps/sdl/camera_capture.cpp` behind
  `CameraCapture`. Desktop and Android frontends share the same capture path,
  while the main event loop only configures or submits a frame to the core.
- SDL audio-device ownership, queue trimming, fast-forward downsampling, and
  sample submission now live in `apps/sdl/audio_output.cpp` behind
  `AudioOutput`. Emulator sample production remains in the core, while the
  frontend owns only playback policy and can clear stale latency consistently
  across pause, state-load, rewind, and link transitions.
- SDL window, renderer, framebuffer textures, gamepad, camera, audio, and
  presentation-state ownership now live in `apps/sdl/sdl_resources.cpp` behind
  `SdlResources`. Construction failures clean up partially-created resources,
  and the event loop retains only policy/state access through the aggregate.
- SDL preference paths, window geometry persistence, ROM-library migration,
  quick-state atomic writes, and printer-image persistence now live in
  `apps/sdl/desktop_storage.cpp`. These filesystem and platform concerns no
  longer share the SDL event loop's dashboard and emulation policy code.
- Asynchronous SDL ROM file-dialog state and result collection now share that
  module, keeping callback synchronization and dialog lifecycle out of the
  main event-loop translation unit.
- Shared SDL event-to-window routing now lives in `apps/sdl/window_event.cpp`;
  TAS, sprite, cheat, and debugger windows use the same event ownership rule,
  covered by a focused SDL contract test.
- Shared tool-window hit testing, button styling, and unsaved-change
  confirmation now live in `apps/sdl/tool_window_support.cpp`, removing UI
  helper duplication from the editor/debugger code paths.
- The TAS editor window now lives in `apps/sdl/tas_editor.hpp` behind its
  existing request/state API. Its frame editing, event handling, rendering,
  and lifecycle are isolated from the SDL event loop; the same extraction
  boundary is now used by the other desktop tools.
- The live sprite editor now lives in `apps/sdl/sprite_editor.hpp` behind its
  existing patch/request API. VRAM snapshots, tile editing, patch import/export,
  IPS generation, event handling, and rendering are isolated from the main
  loop, with the same shared event and tool-window support as the other tools.
- The desktop cheat manager now lives in `apps/sdl/cheat_manager.hpp` behind
  its existing load/apply/request API. Cheat persistence, archive fetching,
  editing, event handling, and rendering are isolated from the main loop.
- The desktop debugger now lives in `apps/sdl/desktop_debugger.hpp` behind its
  existing stepping/request API. Register editing, framebuffer preview,
  execution controls, event handling, and rendering are isolated from the
  application loop; remaining SDL work concerns lifecycle integration and
  keeping concrete-only tools behind explicit capability adapters.
- Android touch geometry, hit testing, held-button ownership, voxel gestures,
  and settings refresh now live in `apps/sdl/android_touch_input.cpp`. The
  platform event loop keeps only sequencing and navigation decisions, making
  touch regressions easier to isolate from desktop input behavior.
- Binding configuration state and its controls dialog now live in
  `apps/sdl/input_configuration.cpp`, while battery flushing and rumble/focus
  cleanup live in `apps/sdl/input_lifecycle.cpp`. These policy helpers are
  shared through small headers instead of being defined beside the main loop.
- The extracted binding state now has focused coverage in
  `tests/sdl_input_configuration_tests.cpp`; the SDL CTest set includes this
  contract alongside input mapping and window-event routing tests.
- Keyboard/gamepad mapping, reserved shortcut filtering, and consistent button
  release behavior now live in `apps/sdl/input_mapping.cpp`; focused SDL input
  contract coverage prevents event-loop changes from silently altering control
  semantics.
- Android library/settings JNI entry points and cross-thread ROM/back requests
  now live in `apps/sdl/android_bridge.cpp`. The SDL event loop consumes a
  small request API, while Java-facing symbol names and settings serialization
  stay isolated from desktop builds.
- Android Libretro metadata lookup, CRC fallback, cover URL construction,
  bounded downloads, bitmap decoding, and cache cleanup now live in
  `android/app/src/main/java/com/danielseim/gbb/ArtworkService.java`.
  `LibraryActivity` receives UI-thread callbacks and no longer owns network or
  file-cache lifecycle.
- Android touch-layout editing now lives in the standalone
  `TouchLayoutView.java`. The activity owns dialog/navigation composition while
  the custom view owns layout math, drawing, hit testing, and drag gestures;
  this makes touch-editor regressions independently reviewable.
- Android library and settings composition now live in `LibraryScreen.java`
  and `SettingsScreen.java`. `LibraryActivity` retains lifecycle, navigation,
  ROM-picker, and JNI bridge ownership, reducing cross-feature UI changes to a
  small activity surface while preserving the existing native method names.
- Logger file/memory sinks and structured context formatting now have a
  dedicated `tests/logging_contract_tests.cpp` executable; diagnostics no
  longer rely on the monolithic core test binary for coverage.
- Save-state primitive wire behavior now has a dedicated
  `tests/save_state_format_tests.cpp` executable covering endian order,
  bounded reads, invalid values, size limits, and the CRC32 check vector.
  The sanitizer configuration also checks this boundary; it caught and
  prevented a temporary-buffer lifetime bug in the initial test fixture.
- Save-state framing is isolated from hardware serialization in
  `src/save_state_container.cpp`; its fixed-header, ROM-identity, size, and
  checksum contract is covered by the same format test executable.
- Touch ownership, desktop dashboard navigation, voxel profile persistence,
  and audio queue helpers now have a dedicated
  `tests/frontend_contract_tests.cpp` executable, further shrinking the
  monolithic hardware-core test surface.
- The remaining core smoke test file now focuses on basic cartridge, camera,
  memory-map, and emulator behavior; subsystem contracts are covered by
  separate CTest executables for faster failure localization.
- SGB command decoding now lives in `src/ppu_sgb.cpp`, separating the packet
  protocol and border/attribute state updates from the main PPU timing and
  scanline implementation without changing the public PPU API.
- SGB protocol coverage is now isolated in `tests/ppu_sgb_contract_tests.cpp`;
  transfer guards, mask-mode bounds, PAL01 rendering, and default-palette
  behavior run as a focused CTest target instead of only through the monolithic
  core test.
- ROM header metadata, filename-language parsing, recency ordering, persistence,
  and fingerprint deduplication now run in the focused
  `tests/rom_library_contract_tests.cpp` target rather than the hardware-core
  test executable.
- Core registry probing, capability descriptors, state/video contracts, and
  factory validation now live in the focused frontend contract target, removing
  another generic-API test cluster from `tests/core_tests.cpp`.
- Equal-confidence core probe selections now emit a warning identifying both
  candidates and the deterministic winner, so registration-order decisions
  are visible in diagnostics instead of being silent.
- Scene snapshot construction and JSON export now run in that same focused
  frontend target, keeping presentation-schema regressions separate from the
  hardware-core test executable.
- GameShark parsing, Libretro cheat serialization, and memory application now
  run in `tests/gameshark_contract_tests.cpp`, isolating cheat-format failures
  from CPU and bus regression failures.
- Cycle-integrated APU resampling coverage now runs in the focused
  `tests/apu_cycle_contract_tests.cpp` target, keeping short-transition audio
  timing failures separate from the larger waveform fixture suite.
- Link timeout/progress accounting now lives in the core-neutral
  `gbb::LinkWatchdog`; Game Boy `LinkSession` supplies only transfer activity
  and progress counters, making the policy reusable by future cores and
  transports.
- DMG/CGB waveform rendering, fixture parsing, tolerance comparison, and
  reference capture now run in the focused
  `tests/apu_waveform_contract_tests.cpp` target. The target copies the six
  checked-in fixtures beside its executable and preserves the
  `GBB_AUDIO_REFERENCE_DIR` and `GBB_AUDIO_REFERENCE_CAPTURE_DIR` controls.
- MBC1, MBC2, MBC3, and MBC5 banking, RTC, battery, and rumble behavior now
  run in the focused `tests/cartridge_mapper_contract_tests.cpp` target,
  separating mapper failures from CPU/PPU and general cartridge tests.
- Serial transfers, local-cable timing, interrupt handshakes, link-session
  lifecycle/retry, packet framing, and TCP loopback behavior now run in
  `tests/serial_link_contract_tests.cpp`, isolating transport regressions from
  the remaining hardware execution tables.
- 16-bit instruction timing, memory addressing, stack/call/return behavior,
  interrupt and low-power states, rotate/bit matrices, and base-opcode
  completeness now run in `tests/cpu_execution_contract_tests.cpp`, isolating
  decoder/control-flow regressions from the core hardware suite.
- Divider/TIMA frequencies, overflow/write edge behavior, timer integration,
  PPU mode and STAT timing, VBlank/frame publication, rendering, joypad input,
  DMA, and CPU machine-cycle timing now run in the focused
  `tests/timer_contract_tests.cpp` and `tests/ppu_timing_contract_tests.cpp`
  targets, keeping timing/video regressions out of the remaining core suite.
- APU power/register behavior, active wave-RAM access, high-pass filtering,
  pulse length/sweep, wave output, and noise output now run in
  `tests/apu_hardware_contract_tests.cpp`, completing isolation of the primary
  audio hardware contract from the core suite.
- Cartridge file-loading coverage now runs in
  `tests/cartridge_file_contract_tests.cpp`, separating temporary-file and ROM
  ingestion failures from mapper behavior tests.
- Game Boy Printer protocol, image conversion, BMP encoding, and serial
  attachment coverage now run in `tests/printer_contract_tests.cpp`, isolating
  peripheral failures from CPU and cartridge tests.
- CPU flag normalization and the exhaustive register-to-register load matrix
  now run in `tests/cpu_register_contract_tests.cpp`, separating register-state
  invariants from the opcode execution tables.
- Immediate-load opcode coverage now shares that focused CPU register target,
  keeping operand-loading invariants out of the monolithic execution suite.
- Register, immediate, and memory ALU tables now run in
  `tests/cpu_alu_contract_tests.cpp`, isolating arithmetic flag regressions from
  the remaining control-flow and timing tables.
- Increment/decrement register and `(HL)` flag-edge cases now share the ALU
  contract target, keeping half-carry/borrow regressions localized.
- Save-state primitive serialization (bounded reader/writer, little-endian
  encoding, and CRC32) now lives in `src/save_state_format.hpp`; the emulator
  codec retains ownership of hardware field ordering and version migration.
  `tests/save_state_format_tests.cpp` covers this format boundary independently
  from full emulator round trips.
- `SaveStateError` now has its own `gameboy/save_state_error.hpp` contract, so
  persistence utilities do not need to include the complete emulator class.
- SDL TCP link transport state now lives in
  `apps/sdl/remote_link_session.hpp`; the event loop consumes a small
  frontend-neutral aggregate instead of declaring transport ownership beside
  unrelated window and input state.
- Desktop input recording/replay and TAS movie serialization now live in
  `apps/sdl/input_movie.cpp` behind `InputMovie`; the main SDL translation
  unit no longer owns the movie file format or replay state machine.
- `tests/input_movie_contract_tests.cpp` now exercises recording, replay,
  fingerprint validation, and clean shutdown independently of the SDL event
  loop, including under ASan/UBSan.
- Link-harness scenario frame traces now use buffered I/O with a periodic
  checkpoint instead of flushing every emulated frame; lifecycle, ownership,
  and stall events remain immediately durable so diagnostics do not perturb
  the serial timing they are measuring.
- Link-harness scenario state and file/report primitives now live in
  `apps/link_harness/scenario_state.*` and `apps/link_harness/harness_io.*`.
  The main harness keeps orchestration and Pokémon-specific decisions, while
  automation state, scenario naming, hashing, report output, and framebuffer
  capture have independently testable build boundaries.
- Link-harness semantic classification now lives in
  `apps/link_harness/semantic_tracker.*`. Party snapshots and guest-memory
  probing are converted into a stable semantic sample at the harness boundary;
  trade/battle detection, failure reasons, and report fields are now isolated
  from transport execution and covered by focused support tests.
- Shared scenario frame policy now lives in `apps/link_harness/scenario_runner.*`.
  Local and TCP execution provide transport-specific frame callbacks, while
  frame limits, counting, and expectation-based early termination are enforced
  consistently and covered by the support target.
- Link-harness trace-file creation, header/termination records, frame
  checkpoints, and flush policy now live in
  `apps/link_harness/scenario_trace_writer.cpp`; Pokémon-specific field
  formatting remains in the harness, keeping the on-disk schema unchanged
  while making future transports and scenarios easier to add.
- The web frontend now receives `GBB_VERSION` from CMake and uses it for SDL
  application metadata instead of reporting a stale hard-coded version.
- SDL frame pacing and remote-link polling now live in
  `apps/sdl/frame_pacer.*`; the main loop delegates deadline/catch-up policy
  instead of embedding wall-clock sleeps alongside emulation and rendering.
  The pacing contract has a focused regression test.
- SDL ROM replacement and local/remote link-session lifecycle now live in
  `apps/sdl/emulation_session.*`; printer switching, battery-save boundaries,
  link retries, split-screen setup, and link-trace lifecycle are kept together
  instead of being interleaved with event dispatch and presentation.
- SDL title formatting, palette/video selection dialogs, help/about screens,
  exit confirmation, and platform-specific error presentation now live in
  `apps/sdl/dialogs.*`. The main loop retains only orchestration and state
  transitions, making UI policy changes easier to review without touching
  emulation or event routing.
- SDL dashboard item construction, label sanitization, hit testing, scrolling,
  and action dispatch now live in `apps/sdl/dashboard_controller.*`. The
  renderer and event dispatcher share one dashboard contract, while ROM and
  settings actions remain independently reviewable.
- SDL optional-tool access now goes through the capability-checked
  `CoreServices` view in `apps/sdl/core_capability.hpp`. Debugger, sprite
  editor, GameShark, and link-cable paths cannot obtain a concrete Game Boy
  adapter unless the owning `EmulatorCore` advertises the corresponding
  capability; voxel input likewise relies on the generic scene-layer
  capability. This is the first migration slice toward fully generic
  advanced services without introducing a dynamic plugin ABI.
- SDL's main-loop lifecycle now keeps one `CoreServices` view per iteration and
  refreshes it whenever a ROM is replaced. Camera setup, debugger/TAS stepping,
  sprite editing, GameShark requests, and local/remote link startup all use the
  same capability-gated view, preventing stale adapter pointers and duplicated
  capability checks during transitions.

The headless core suite, SDL desktop target, and Android Java frontend all
build successfully after this pass.
