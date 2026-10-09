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
The diagnostic timer-poll hook services changed mailbox tokens before reading
timer 0. The standalone engine image and admission guards are unchanged.

A separate variant of the original diagnostic SNES host recognizes mailbox CB
and restricts SOUND to score 1 or explicit music stop (`80`), with zero effects
A/B and zero attributes.
The image is a deterministic 256 KiB LoROM, SHA-256
`220d292b4a41d07af3feb8e9375b57a78506b7a52bc2512d87ebc6de18d2850c`.
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
DSP and returns to the bundled IPL. Control 3 stops music and clears a staged
selection. Music code `80` performs the same stop during the control-2 stage. Both stop paths disable timer 0,
clear KON, assert KOF FF and mute/reset DSP before acknowledgment. A pending
control-0 stage then acknowledges without restarting playback.
Unsupported host SOUND fields use the existing bounded stop-and-halt path.

Diagnostic output port 2 reports 0 at cold startup, 1 after silent validation,
3 during rendering, 2 after completion and E2 on rejection. The host mirrors
output ports 1/2/3 at WRAM `$30..32`; existing transfer, adoption, ownership,
command and error counters remain the observation source. These fields are
telemetry, not evidence of general score compatibility.

The timer-poll bridge services commands during active playback. A score-1
reselection discards the old playback call stack, mutes the old voices and
re-enters full validation before starting a fresh score timer. Cooperative IPL
requests capture interruption evidence, clear readiness, stop DSP/timer activity
and enter the loader. Restart must pass silent rehearsal again before adoption.
After a stop, mailbox polling continues with timer reads suppressed; after
natural completion the bridge returns to validated idle mode.

The original completion fixtures retain their 64-frame spacing. Active fixtures
send stop, score-1 reselection, SOU_TRN or an unsupported effect eight LCD frames
after the initial SOUND. No delay is fitted to proprietary behavior. This
qualifies bounded owned interruptions at the tested first-note position; it
adds no general vendor command, multi-song or scheduler compatibility claim.

The bridge reserves direct-page D0..D7 for command state and diagnostic
observations. D3 records active stop/reselection/upload bits 1/2/4, D4 records
which of those commands saw positive voice-2 ENVX, D5 records the interrupted
score tick and D6 records the count. The IPL clears direct page, so a cooperative
handoff preserves D3..D6 at unused `$0500..0503`; `$0400` restores the evidence
before validating the new bank. Initial upload/cold reset clears it. The owned
bank upload touches only `$2B00..32FF`, preserving that diagnostic archive.
These bytes establish the test's interruption position, not vendor behavior.

## Public validation

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_transport_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
python3 scripts/build_sgb_score_transport.py --output /tmp/score-host.rom
python3 scripts/build_sgb_score_transport_fixture.py --output /tmp/score-game.gb
python3 scripts/build_sgb_score_transport_fixture.py \
  --interruption upload --output /tmp/score-active-upload.gb
```

Both image exporters refuse to overwrite files. No proprietary firmware is a test
input. The whole-host probe compares ordinary execution with a separately
constructed host restored during upload, external restart, validated readiness,
rendering, interruption and completion. Every restoration must reproduce the
exact serialized state; snapshot triggers include changed interruption masks and counts. PCM
and final whole-host state must match the uninterrupted run. Cold
reset must reproduce them as well.
The probe uses an owned three-byte Game Boy boot jump to `$0100`; it does not
extend title bootstrap or cartridge-header qualification.

The public suite covers silent validation before SOUND, first selection,
reselection without upload, repeated upload/restart/reselection, malformed-root
rejection, unsupported song IDs 2/3, both nonzero effect fields and nonzero
attributes on SGB1 and SGB2. Active stop, stop followed by reselection,
reselection, upload and unsupported effect commands are tested on both models in all three audio modes. Every
active case requires exactly one interruption, positive ENVX and a score tick
between 0 and 32; stopped cases must retain DSP KOF FF and FLG E0 and
a frozen score clock. Restarted/reselected scores must finish at tick 72.
The probe uses read-only physical SPC RAM and DSP observations, without exposing mutable
processors or injecting state. Native and scalar output must match exactly.
Combined 48 kHz output has its own exact reset/save/load comparison and bounded
resampling counts relative to native 32 kHz output. Successful scores must
produce nonzero samples and leave at least one million physical master clocks
of silent tail.

All seven public integration tests pass across 54 model/mode/scenario runs,
each with uninterrupted, restored and cold-reset executions. The seven related
host, transfer, firmware, fixture and native short-pair CTest suites also pass.
The bundled prototype hash check and `git diff --check` pass.

The probe caps execution at 140 million master clocks and 4096 restorations;
the test matrix uses at most 100 million clocks, caps output at 4096 bytes and
250000 PCM frames, and bounds each child to 90 seconds. The CTest suite has a
600-second limit. Neither program images nor PCM artifacts are checked in.

## Multi-song milestone

The separate [CC directory diagnostic](sgb-native-score-directory.md) now admits
three distinct owned song roots and supports selection/switching of songs 2/3.
The CB image and its single-song contract remain reproducible as documented
above. The separate [two-instrument profile](sgb-native-score-dual-instrument.md)
now adds owned uploaded samples, ordered E0 selection and voice-local inheritance.
The [multi-block sample profile](sgb-native-score-brr-chain.md) now adds bounded
BRR chains and checked loop points. The [D0 profile](sgb-native-score-instrument-profiles.md)
adds bounded uploaded envelopes/direct GAIN and tuning. The D1 profile below adds non-looping BRR samples; general vendor-bank and
real-title qualification remain open.

The [D1 one-shot profile](sgb-native-score-one-shot.md) now covers bounded
non-looping samples and natural completion. The next sample extension is
bounded BRR filter/range profiles.
