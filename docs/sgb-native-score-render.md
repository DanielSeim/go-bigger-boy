# Experimental native DSP renderer

`firmware/sgb/score_render.asm` extends the isolated
[native single-track parser](sgb-native-score-track.md) with actual DSP setup,
key-on and key-off writes. The production SPC CPU schedules and renders an
owned flat stream through `SnesApuAudioEngine`. An independently authored,
one-block looping BRR square wave supplies the audio. No private samples are
copied or loaded by the native probe.

This is a standalone diagnostic renderer, excluded from bundled firmware and
production ownership. SGB1/SGB2 program ROMs remain required. The report's
`playback: false` and `qualification: false` describe production vendor playback;
the experiment itself emits owned PCM.

## Rendering profile

The existing flat-track interface and limits remain: bytes at `$2B00`, length
1..128 in `$20`, tempo 96 or 192 in `$12`, at most 16 events and exact zero
termination. Before timing or audio starts, the renderer additionally requires
articulation `$7F`, duration 2..127 and notes `$98/$99/$A4` or rest `$C9`.
Unsupported pitches or articulation, malformed streams and controls reject
without event records, key-ons or nonzero PCM, including an unsupported event
after an otherwise valid prefix.

Physical voice 2 uses source 0, left/right volume 80, direct gain 127 and ADSR
disabled. DIR is `$10`, pointing to an owned directory at `$1000` and looping
BRR block at `$1010`. Master volumes are 127; echo volumes, routing, noise and
pitch modulation are zero. FLG `$20` disables echo writes while permitting
output. The three pitch registers are 1068, 1132 and 2140, using the earlier
sanitized pitch observations. These values do not establish a general pitch
conversion table or vendor instrument mapping.

Gates are independently defined as full event duration. Each transition
asserts key-off; a new note holds that assertion for over two DSP frames, sets
pitch, clears key-off and keys on. Rests remain keyed off. Completion asserts
key-off and disables the score timer while the DSP continues its release tail.
This is not the original articulation algorithm or envelope. Renderer scratch
byte `$2C` extends the parser's private diagnostic RAM footprint; no integrated
memory ownership is claimed.

## Build and evidence

```sh
python3 scripts/build_sgb_score_render.py --output /tmp/score-render.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_render_probe
ctest --test-dir build-dmg-firmware -R '^gameboy_sgb_native_score_render$' --output-on-failure
python3 scripts/check_sgb_score_render_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_render_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The builder applies checked source hooks to the clock/parser and appends owned
renderer code, directory and BRR bytes through the strict assembler. It writes
2073 bytes at `$0800`, SHA-256
`4a323acb4c71ae6189819da020295a6299c5a8a3b390e4bc7732d78964f88b4b`.
Existing output files are preserved, and the separate parser and clock builds
retain their original hashes. Bundled firmware is unchanged.

The native probe directly installs owned inputs through an owned IPL trampoline;
it does not exercise upload, whole SGB boot or frontend audio. It observes
DSP register key-ons without replacing the engine's owned write observer.
Checks cover voice mask, pitch/source/gain/volume/DIR, disabled echo/noise,
symbolic event alignment and nonzero equal stereo output. Whole-engine state
restores must reproduce physical-half event/key-on timestamps and PCM fingerprints;
reset must reproduce the same result. The probe checks a quiet release tail,
rest-only silence and quiet interior rests, inherited durations, maximum event
count, counter carry,
unsupported tempo and fail-closed stream rejection.

PCM reports contain frame counts, peak amplitude and a deterministic FNV-1a
fingerprint of the observed interleaved little-endian stereo samples. No private
PCM is captured or compared. A native fixture starts with a duration-16 rest,
plays the three measured pitches, then rests for eight ticks. The optional
private gate compares its actual DSP key-on spacings with six fresh original
SGB1/SGB2 baseline, double-tempo and short-articulation cases within one timer
pulse (2048 SPC cycles). The fixed native articulation is compared only for
onset spacing against the short-articulation reference, not its gates or PCM.
Startup phase and absolute first-note alignment remain unqualified.

The next step is an independently specified bounded articulation gate backed
by controlled timing evidence. Vendor instrument/sample mapping, controls,
multiple tracks, calls, phrase transitions and production integration still
need separate implementation and validation.

The isolated [calibrated gate experiment](sgb-native-score-gate.md) now covers
three duration-16 tempo/articulation profiles, preserving this renderer build.
