# Experimental native instrument and volume controls

`firmware/sgb/score_controls.asm` adds E0 instrument selection, E5 song volume
and ED track volume to an isolated native single-track renderer. The SPC reads
and applies these controls; the host probe supplies owned inputs and observes
results. The complete stream is validated before rendering starts. This build
remains excluded from bundled firmware and production ownership; SGB1/SGB2
program ROMs remain required.

## Bounded control profile

| Control | Accepted values |
| --- | --- |
| E0 instrument | 2, 10 |
| E5 song volume | 160, 80 |
| ED track volume | 127, 64 |

The build accepts tempo 96 only, articulation `$7F`, notes `$98/$99/$A4` of
16 ticks, and rests of 2..127 ticks. Default instrument/song/track state is
2/160/127. Track startup clears control state before both validation and replay.
Controls may appear before the first duration or between events; duration,
articulation and controls are inherited until changed. Values apply to the next
note's DSP setup. Live changes to an already sounding note are not qualified.

At each note, instrument 2 accepts volume pairs (160,127), (80,127) and
(160,64), producing centered voice volumes 7, 1 and 1 respectively. The combined
reduction (80,64) rejects. Instrument 10 accepts the full pair (160,127) only;
its owned centered volume is 7. Volume reference matching is limited to
instrument 2. Rest-only streams may retain otherwise unsupported combinations
because no note uses them. Unknown control values, unsupported opcodes, missing
operands and unsupported note combinations reject before events or audio,
including when they follow a valid prefix. All reads remain bounded by the
1..128-byte input and the 16-event limit; control-only streams cannot produce
an empty successful track. Pan, tempo commands, echo and other controls remain
unsupported.

## Instrument setup and owned source

| Instrument | Note pitches 24/25/36 | SRCN | ADSR1 | ADSR2 | GAIN |
| --- | --- | --- | --- | --- | --- |
| 2 | 1068 / 1132 / 2140 | 2 | `$8F` | `$6F` | `$B8` |
| 10 | 7993 / 8472 / 16016 | 10 | `$8E` | `$AF` | `$B8` |

These are bounded sanitized register observations, not a copied resident
instrument table or general pitch conversion. Both directory slots point to
one independently authored looping square-wave BRR block at `$1100`; no vendor
samples are installed. The directory starts at `$1000`. Source identifiers match
the observed registers, but sample contents and audible timbre are independently
defined. ADSR is enabled; the observed GAIN byte is written even though ADSR
selects envelope behavior. Voice 2, master volumes, disabled echo/noise, key-off
handling and the diagnostic mailbox isolation remain as in the gated build.

Private direct-page `$30/$31/$32` holds instrument/song/track state. The control
build reuses the duration-16 baseline gate pulse profile. Its broader gated
sibling retains all ten measured timing profiles and its image hash. There is
no claim of integrated memory ownership or original control-transition timing.

## Build and evidence

```sh
python3 scripts/build_sgb_score_controls.py --output /tmp/score-controls.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_controls_probe
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_native_score_controls$' --output-on-failure
python3 scripts/check_sgb_score_controls_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_controls_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The source builder produces 2313 bytes at `$0800`, SHA-256
`2367ca355562edd645a0c753dcc574673427c0ed25c8c5b101224ecdcf6b5a6d`.
Output files must not already exist. Standalone clock, parser, full-duration
renderer and ten-profile gated images retain their hashes. Bundled firmware
is unchanged.

ROM-free checks compare control inheritance with the independent symbolic
oracle, verify changes between instruments and volume pairs, require reduced
volume to lower owned PCM peak amplitude, and check rest-only silence and quiet
release. Reset and whole-engine save/load must reproduce physical-half event,
key-on and key-off timestamps, setup registers and owned PCM fingerprints.
Malformed or unsupported input must produce no events, key-ons or nonzero PCM.
The native probe uses an owned IPL trampoline and direct installation; upload,
whole SGB boot and frontend audio remain outside this diagnostic check.

Six fresh original SGB1/SGB2 runs provide eighteen reference note snapshots:
instruments 2/10 with the three pitches on each model, and the three centered
volume pairs on each model. The comparator matches pitch/source/ADSR/gain for
instrument fixtures and pitch/source/left/right volume for volume fixtures.
The original volume fixture restores complete state in three separate song
requests; native fixtures use one flat stream. Register snapshot agreement does
not qualify live control transitions, control-command timing or original PCM.
Only sanitized register metadata, hashes and owned PCM statistics are exported.
Reports retain `qualification: false` and `playback: false`.

The next step is bounded pan control for the measured center/endpoints, followed
by controlled multi-track state and phrase integration. Broader instruments,
volume curves and production vendor playback still need separate evidence.
