# Owned score upload, restart and selection

The guarded [consecutive duration-4 engine](sgb-native-score-short-pair.md)
now has an opt-in whole-host diagnostic integration. An independently authored
Game Boy cartridge uploads its 2048-byte bank through an actual SOU_TRN LCD
transfer, enters the original `$0400` restart bridge, waits for fresh readiness,
and selects score 1 through SOUND. The diagnostic uses the existing emulated
65C816, ICD, SPC700 IPL, APU and DSP; it does not inject score RAM or synthesize
host-side audio.

This is a transport and lifecycle milestone. Qualification and production
playback remain false. SGB1/SGB2 program ROMs remain required in production;
the bundled prototype and external-image overrides are unchanged. It establishes
no additional proprietary-bank, real-title, hardware or acoustic equivalence.
Performance optimization remains deferred.

## Build and admission

`scripts/build_sgb_score_transport.py` assembles the independently written
`firmware/sgb/score_transport_bridge.asm` and a diagnostic variant of the guarded
4096-byte engine. The bridge occupies `$0200..03FF`, restart enters `$0400`,
and the engine retains its fixed `$0800..17FF` allocation. The combined payload
is bounded to `$0200..17FF`. Named source hooks redirect engine setup,
completion and rejection; every hook must match exactly once or the build fails.
The standalone engine image and admission guards are unchanged.

A separate variant of the original diagnostic SNES host recognizes mailbox CB
and restricts SOUND to score 1 with zero effects A/B and zero attributes.
The image is a deterministic 256 KiB LoROM, SHA-256
`0cf3e20bb7386d3862260dad338e51da4153305380182f92b0c182981fe10db8`.
It is exported on request, never selected or bundled for production.

The cartridge builder `scripts/build_sgb_score_transport_fixture.py` reuses the
owned short-pair bank and the existing planar upload fixture. Its block list
uploads exactly 2048 bytes at `$2B00`, then hands off to `$0400`; the full LCD
payload remains 4096 bytes. Directory word 1 identifies the guarded root.
Malformed-root fixtures are independently generated negative tests.

## Fresh readiness and commands

Cold startup advertises output ports 0/1/3 as `5A/CB/A5` solely to establish the
cooperative upload contract. It does not admit SOUND before a bank is validated.
The restart bridge clears the version and signature before entering the native
parser and silent rehearsal. Only the fully admitted profile republishes
`5A/CB/A5`. A malformed bank remains muted with version/signature zero and
output port 2 = E2, retaining external ownership.

Adoption requires the host to clear all input ports and receive a fresh token-0
acknowledgment. The subsequent control-2 token stages score 1; the separate
control-0 token requires zero effects before starting native playback. Each
stage acknowledges its own changed token. Control 1 clears readiness, mutes
DSP and returns to the bundled IPL. Control 3 clears a staged selection.
Unsupported host SOUND fields use the existing bounded stop-and-halt path.

Diagnostic output port 2 reports 0 at cold startup, 1 after silent validation,
3 during rendering, 2 after completion and E2 on rejection. The host mirrors
output ports 1/2/3 at WRAM `$30..32`; existing transfer, adoption, ownership,
command and error counters remain the observation source. These fields are
telemetry, not evidence of general score compatibility.

Playback currently blocks mailbox polling until the finite admitted score ends.
The cartridge therefore waits 64 LCD frames between operations; repeated
uploads add a second 64-frame interval before reselection. Stop, selection and
upload during active playback are not qualified. There is no claim that this
spacing reproduces a proprietary scheduler or permits real-title integration.

## Public validation

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_transport_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
python3 scripts/build_sgb_score_transport.py --output /tmp/score-host.rom
python3 scripts/build_sgb_score_transport_fixture.py --output /tmp/score-game.gb
```

Both image exporters refuse to overwrite files. No original firmware is a test
input. The whole-host probe compares ordinary execution with a separately
constructed host restored during upload, external restart, validated readiness,
rendering and completion. Every restoration must reproduce the exact serialized
state; PCM and final whole-host state must match the uninterrupted run. Cold
reset must reproduce them as well.
The probe uses an owned three-byte Game Boy boot jump to `$0100`; it does not
extend title bootstrap or cartridge-header qualification.

The public suite covers silent validation before SOUND, first selection,
reselection without upload, repeated upload/restart/reselection, malformed-root
rejection, unsupported song IDs 2/3, both nonzero effect fields and nonzero
attributes on SGB1 and SGB2. Native and scalar output must match exactly.
Combined 48 kHz output has its own exact reset/save/load comparison and bounded
resampling counts relative to native 32 kHz output. Successful scores must
produce nonzero samples and leave at least one million physical master clocks
of silent tail.

All six public integration tests pass, covering 24 model/mode/scenario runs,
each with uninterrupted, restored and cold-reset executions. The seven related
host, transfer, firmware, fixture and native short-pair CTest suites also pass.
The bundled prototype hash check and `git diff --check` pass.

The probe caps execution at 140 million master clocks and 4096 restorations;
the test matrix uses at most 100 million clocks, caps output at 4096 bytes and
250000 PCM frames, and bounds each child to 90 seconds. The CTest suite has a
600-second limit. Neither program images nor PCM artifacts are checked in.

## Next step

Make active playback service the mailbox at bounded scheduler boundaries.
Validate stop, score-1 reselection and cooperative IPL upload while a note is
active, requiring correct acknowledgments, physical DSP silence and fresh
readiness after restart. Keep admission and timing guards intact and repeat
SGB1/SGB2 reset/save/load checks at those interruptions. This addresses the
current blocking integration boundary before expanding song directories or
vendor instrument/sample compatibility.
