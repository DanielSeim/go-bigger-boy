# Experimental per-voice mix controls

The isolated mix renderer adds measured pan and volume points to
[chromatic finite-call playback](sgb-native-score-chromatic.md). Channels 2/3
keep independent pan and track volume through timed notes, subroutine repeats
and returns. Song volume is a shared startup setting. This native diagnostic
remains outside bundling and production selection; SGB1/SGB2 program ROMs
are still required.

## Accepted controls

| Control | Accepted values and placement |
| --- | --- |
| E0 instrument | 2, caller prefix only |
| E1 pan | 0, 10, 20; caller or callee, before/between events |
| ED track volume | 127, 64; caller or callee, before/between events |
| E5 song volume | 160, 80; at most once, first-pattern channel-2 prefix |
| E7 tempo | Supplied tempo 96/128/192, caller prefix only |

Pan/track defaults are 10/127 at each track and pattern startup; song defaults
to 160 and persists across patterns. Within each expanded track, pan and track
volume inherit into calls, between body executions and after returns. Changes
select the next note's setup. Cross-pattern inheritance and live global song
volume changes are not qualified; the owned reference fixtures explicitly set
pan/track in their second pattern.

| Pan | Song / track volume | DSP VOLL / VOLR |
| --- | --- | --- |
| 0 | 160 / 127 | 0 / 11 |
| 10 | 160 / 127 | 7 / 7 |
| 10 | 80 / 127 | 1 / 1 |
| 10 | 160 / 64 | 1 / 1 |
| 20 | 160 / 127 | 11 / 0 |

These are the previously measured instrument-2 points, now checked in coupled
playback. Combined reductions (80/64), endpoint reductions, intermediate or
flagged pan operands, other instruments and unsupported controls reject before
any live events or PCM. Rests may retain otherwise unsupported combinations;
they have no volume write or KON. Defaults still select instrument-2 pitch words
when no E0 is supplied. There is no generalized volume/pan law.

The existing bounds remain: 1..128 bank bytes, exactly two patterns, channels
2/3, 1..8 expanded timed events per track, one nonnested call with counts 1..3,
32 events and 2032 ticks overall. Each expanded track accepts at most 32 executed
control commands, counting body repeats. Fixed prefix commands retain a five-command
bound. Articulations 63/127, the ten measured gate profiles, rests, first-end
clipping, silent timeline rehearsal and complete prevalidation remain unchanged.

## Native DSP ownership

The SPC expands per-event left/right volume, pan, track and shared song state
into parallel caches at `$3400..3800`, alongside the existing duration/opcode,
pulse and articulation caches. Runtime setup writes only the selected voice's
VOLL/VOLR. Pitch setup and combined KON retain the gated chromatic path; peer
KOF, pitch, envelope and volumes remain independent. Direct-page `$81..8D`
holds parser controls and the current event's setup.

The independently authored source is `firmware/sgb/score_mix.asm`, composed by
`build_sgb_score_mix.py`. The reproducible artifact is 2602 bytes at `$0800`,
SHA-256 `210fec34a40227286ae3bc5c6006b43580cb4d537c228c2fea4c0f17d28f6667`.
Pitch tables, measured pulse counts and the prior chromatic image are unchanged.
The shared probe exports extra mix fields only for this driver.

Source 0 still uses the owned looping BRR block, direct gain 127, disabled
echo/noise/modulation and master volumes 127. Original instrument-2 SRCN/ADSR
settings are verified on the reference side but are not installed by this
renderer. Original waveform, envelopes and PCM equivalence remain unqualified.
The separate VOLL/VOLR writes can briefly alter a decaying outgoing tail; exact
stereo equality is checked for constant centered settings, not changing-volume
transitions. The native component checks all ten gate profiles without changing
their timing allowances.

Installation remains a direct-RAM diagnostic with an owned IPL trampoline.
This is not whole-system upload, boot, GUI or production playback qualification.
The bundled prototype and external firmware overrides are unchanged.

## Owned checks

```sh
python3 scripts/build_sgb_score_mix.py --output /tmp/score-mix.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_mix_probe
ctest --test-dir build-dmg-firmware -R 'native_score_mix$' --output-on-failure
```

The ten ROM-free tests cover actual DSP onset volumes and chromatic pitches,
all measured gate profiles, every repeat count, caller/callee/return inheritance,
independent asynchronous updates, first-end clipping, rest-only silence and
exact 32-command expansion bounds. An independent symbolic scheduler checks
inherited settings. Constant endpoints require zero frames and peak in their
inactive lane; constant centered playback requires equal stereo samples.
Both centered reductions lower owned PCM peak amplitude.

Settled release checks observe each expired voice's ENVX=0 while its active
peer stays at 127. Exclusive opposite endpoints additionally require silence
in the expired physical PCM lane while the peer remains audible. Centered
voices share physical lanes, so those checks use per-voice envelopes and DSP
registers. Reset and whole-engine/cross-engine restore compare events, actual
KON/KOF timestamps, setup snapshots, completion and owned PCM fingerprints.
Every prefix truncation of both fixtures and malformed/unsupported commands
must reject silently. Observer/comparator tests reject missing KOF, incorrect
volumes/pitches, malformed metadata and duplicate/missing model runs.

The new suite and seventeen existing score/fixture regression suites passed.
The bundled prototype passed its reproducibility check.

## Private black-box evidence

`build_sgb_mix_fixture.py` authors two 125-byte banks delivered through actual
JOYP/SOU_TRN transport. Both voices use instrument 2 and duration 16 at tempo
96. Their onset notes are 24,25,26,25,26,36,29,32. Each case runs with
articulations 63/127 on both original models:

- `pan-calls`: start at opposite endpoints; a twice-executed shared body
  selects center/full then center/track-reduced settings. The caller inherits
  the last reduced setting after return. Pattern two restores full track volume
  and swaps the endpoints.
- `song-calls`: select shared song volume 80 once; both channels, repeated body,
  return notes and pattern two retain the centered 1/1 setting.

| Case | Articulation | Owned cartridge SHA-256 |
| --- | --- | --- |
| pan-calls | 127 | `7d7889c2f2cfbb865506671550292ebf5ad46a98915cf947424057756cc786b7` |
| pan-calls | 63 | `d62727d2ebc8d7efc7a259d1c4751adeeeaecc4523ed91fff2e80e35b89ac701` |
| song-calls | 127 | `768b3e0a1049de65e789361782d18d9e79aa03713e285484eb46c4da0aaa27e8` |
| song-calls | 63 | `e90971ac29bb7fce3e30b0959609e4855948b19830919d677f8a624e4ad3c2f4` |

```sh
python3 scripts/check_sgb_score_mix_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_mix_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

All eight fresh comparison runs passed, with 128 directly observed per-voice
KOF releases. At every KON, native pitch and volume pairs match the pinned
original register settings exactly. Native/original DSP onset spacing differed
by at most 1974 SPC cycles, gate lengths by 2918, and original model onset
intervals by 464. The existing allowances remain 4096 cycles for native/original
onsets and gates, 2048 between original models, and 84000..92000 for original
fixture onset spacing. Missing releases cannot be inferred from retriggers.

Private files are executed as opaque references; no firmware instructions,
instrument tables, private scores or samples are inspected or incorporated.
Runs retain the 8000000-instruction bound, 180-second child timeout, 16-MiB
trace bound and fewer than 32768 rows. Temporary raw traces are discarded;
only sanitized register/timing metadata and hashes are exported. No original
PCM, physical-device or independent-emulator comparison was performed.
Fresh coupled evidence covers tempo 96/duration 16; other profiles and
asynchronous updates have component evidence.

Schemas are `gbb-spc-score-mix-v1` and `gbb-score-mix-reference-v1`, with
qualification and playback false. Broader instruments, control transitions,
channel/phrase grammar and production integration remain ahead.
