# Experimental native instrument, volume and pan controls

`firmware/sgb/score_controls.asm` adds E0 instrument selection, E1 pan, E5 song
volume and ED track volume to an isolated native single-track renderer. The SPC reads
and applies these controls; the host probe supplies owned inputs and observes
results. The complete stream is validated before rendering starts. This build
remains excluded from bundled firmware and production ownership; SGB1/SGB2
program ROMs remain required.

## Bounded control profile

| Control | Accepted values |
| --- | --- |
| E0 instrument | 2, 10 |
| E1 pan | 0, 10, 20 |
| E5 song volume | 160, 80 |
| ED track volume | 127, 64 |

The build accepts tempo 96 only, articulation `$7F`, notes `$98/$99/$A4` of
16 ticks, and rests of 2..127 ticks. Default instrument/song/track/pan state is
2/160/127/10. Track startup clears control state before both validation and replay.
Controls may appear before the first duration or between events; duration,
articulation and controls are inherited until changed. Values apply to the next
note's DSP setup. Live changes to an already sounding note are not qualified.

At center pan 10, instrument 2 accepts volume pairs (160,127), (80,127) and
(160,64), producing centered voice volumes 7, 1 and 1 respectively. The combined
reduction (80,64) rejects. Instrument 10 accepts the full pair (160,127) only;
its owned centered volume is 7, and endpoint pan is unsupported. Volume reference matching is limited to
instrument 2. Rest-only streams may retain otherwise unsupported combinations
because no note uses them. Unknown control values, unsupported opcodes, missing
operands and unsupported note combinations reject before events or audio,
including when they follow a valid prefix. All reads remain bounded by the
1..128-byte input and the 16-event limit; control-only streams cannot produce
an empty successful track. Tempo commands, echo, continuous pan curves,
flagged pan operands and other controls remain unsupported.

Pan endpoints require instrument 2 and the full (160,127) volume pair. Pan 0
writes left/right voice volumes (0,11); pan 20 writes (11,0). Pan 10 writes
(7,7) at full volume or (1,1) for the two measured centered reductions. Endpoint
pan combined with reduced volume rejects before audio. Pan state is inherited
and may return to center before a volume reduction or instrument change.
Native tests exercise all three supported pitches; the original pan/volume
reference snapshots are limited to note 24. These points do not establish an
intermediate pan curve or untested instrument/volume combinations.

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

Private direct-page `$30/$31/$32/$33` holds instrument/song/track/pan state,
with `$34` reserved for rendering scratch. The control
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
`ef765134194c407c2cbcb31b8c9cb09e7c539f20118f68afac2fab8ee84806b2`.
Output files must not already exist. Standalone clock, parser, full-duration
renderer and ten-profile gated images retain their hashes. Bundled firmware
is unchanged.

ROM-free checks compare control inheritance with the independent symbolic
oracle, verify changes between instruments, volume pairs and pan points, require reduced
volume to lower owned PCM peak amplitude, and check rest-only silence and quiet
release. Center-only streams must have equal stereo samples; each constant
endpoint stream must produce zero PCM frames and zero peak amplitude in its
inactive channel. Reset and whole-engine save/load must reproduce physical-half event,
key-on and key-off timestamps, setup registers and owned PCM fingerprints.
Malformed or unsupported input must produce no events, key-ons or nonzero PCM.
The native probe uses an owned IPL trampoline and direct installation; upload,
whole SGB boot and frontend audio remain outside this diagnostic check.

Eight fresh original SGB1/SGB2 runs provide twenty-four reference note snapshots:
instruments 2/10 with the three pitches on each model, and the three centered
volume pairs and pan center/endpoints on each model. The comparator matches pitch/source/ADSR/gain for
instrument fixtures and pitch/source/left/right volume for volume and pan fixtures, including the
raw pan value for the latter.
The original volume fixture restores complete state in three separate song
requests; native fixtures use one flat stream. Register snapshot agreement does
not qualify live control transitions, control-command timing or original PCM.
Only sanitized register metadata, hashes and owned PCM statistics are exported.
Reports retain `qualification: false` and `playback: false`.

Native reports use `gbb-spc-score-controls-v2` and reference reports use
`gbb-score-controls-reference-v2`. Per-channel frame counts, peaks and a stereo
identity flag supplement the interleaved owned PCM fingerprint.

The separate [native two-track scheduler](sgb-native-score-pair.md) now checks
the first-ending-track pattern rule while muted. The
[native phrase-bank parser](sgb-native-score-phrase.md) adds bounded pointer
traversal; two-voice renderer integration remains next. Broader instruments, volume
curves and production vendor playback still need separate evidence.

The separate [per-voice mix integration](sgb-native-score-mix.md) now applies
measured instrument-2 pan/volume points to chromatic two-voice playback, with
finite-call inheritance and fresh coupled reference checks. It remains
experimental and does not extend production firmware qualification.
