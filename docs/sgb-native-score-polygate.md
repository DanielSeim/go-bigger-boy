# Experimental measured independent polyphonic gates

The isolated `score_polygate.asm` integration adds separate note gates to the
[independent owned renderer](sgb-native-score-poly.md). It is not bundled;
SGB1/SGB2 program ROMs remain required for production playback.

The bounded raw-bank grammar, two patterns, four events per track, complete
prevalidation and timeline rehearsal remain. Notes are `$98`, `$99`, `$A4`,
rests `$C9`, and articulation is 127 only. Five explicitly measured profiles
are accepted for notes:

| Tempo | Duration | Timer pulses |
| --- | --- | --- |
| 96 | 8 | 15 |
| 96 | 16 | 36 |
| 96 | 24 | 58 |
| 128 | 16 | 27 |
| 192 | 16 | 18 |

These are the articulation-127 entries from the already measured single-track
profiles. There is no interpolation or general duration/articulation law.
Unsupported note profiles reject before timer activation or audio, including
in later or clipped tracks. Rests keep the existing duration bound 2..127 and
start no gate. Tempo 128 is added to the native clock's accepted values; E7 must
still match the externally supplied tempo.

The native parser validates each profile and stores its pulse count in a
parallel cache at `$3200`, alongside the duration/opcode pairs at `$3100`.
This avoids profile lookup during note setup and keeps onset latency bounded.
At actual KON, each newly keyed voice starts its own pulse counter. Every
consumed timer-0 pulse decrements both active counters before fractional
score-tick advancement. Expired counters assert KOF only for their voices,
preserving the peer's held state; simultaneous expiry uses one combined write.
Score transitions cancel only the affected counters. Pattern boundaries and
completion cancel both and release any still active clipped note. Gate expiry
does not change the track's logical duration or event cursor.

The fixed DSP setup remains owned source 0, direct gain 127, voice 2 left-only
and voice 3 right-only at volume 80, with master 127 and echo/noise/modulation
disabled. These gate timing profiles do not qualify original envelopes,
volume/pan laws, waveform timbre or PCM. Installation remains an owned IPL
trampoline/direct RAM diagnostic, outside upload and whole-system boot.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_polygate.py --output /tmp/score-polygate.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_polygate_probe
ctest --test-dir build-dmg-firmware -R 'native_score_polygate$' --output-on-failure
```

The reproducible 2073-byte artifact has SHA-256
`3b35adb6c845b3680922cb994814329b106e2f53ff55ae8038f4794509dd705e`.
All twelve native score CTest suites and the existing phrase-fixture suite
passed. The full-duration renderer remains byte-identical. The production APU CPU,
bus, timers and DSP execute the native driver.

The probe captures actual KON and newly asserted KOF bits, both pitches,
changed/held masks, selected pending pulse counts, physical half-cycle and
logical tick. Release cause 1 is timer expiry; cause 0 is scheduler clipping.
The validator associates every release with its active note. Timer releases
must match the declared pulse profile within one pulse early or bounded late
instruction latency; scheduler releases must clip a note extending beyond a
pattern boundary. A missing gate cannot be substituted by full-duration KOF.
The independent symbolic scheduler still checks logical event timing.

An unaffected active peer must retain clear KOF and ENVX 127 during single-voice
changes or gate expiry. After 10000 SPC cycles from expiry, the expired voice
must produce exactly silent PCM while an active peer continues; settled-frame
and peer-nonzero counts make these windows explicit. The seven-case suite checks
all five profiles on both voices, asynchronous re-key and gate preservation,
simultaneous expiry, both clipped sides, rests, event limits, unsupported late
profiles, ambiguous boundaries and malformed banks. Reset and cross-instance
save/load must preserve every key edge, pulse selection, diagnostic count,
completion time and owned PCM fingerprint, including continuation through each
release. Completion cancels counters and drains to silence.

Private black-box comparison:

```sh
python3 scripts/check_sgb_score_polygate_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_polygate_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

Six fresh SGB1/SGB2 runs use the owned three-onset multi-event banks at tempo 96
with durations 8/16. The observer associates KOF writes with active original
voices. A longer second note clipped by the first pattern can be re-keyed by the
next pattern without an intervening KOF write. That is exported separately as
`release_kind: retrigger`, measuring the onset-to-handoff interval. Direct KOF
observations use `release_kind: keyoff`. Missing release before an ordinary
in-pattern retrigger still rejects; only the known pattern handoff permits this
inference. Original key-off behavior is not equated with native explicit KOF.

The checked six runs contain 32 directly observed KOF note releases and four
re-key handoffs. The comparator matches combined onset masks/pitches and checks
native DSP onset spacing, observed KOF gate lengths and labeled handoff
intervals within 4096 SPC cycles (two timer pulses). This allowance is broader
than the earlier isolated single-track gate check and includes coupled startup
phase and bounded setup latency. Maximum differences were 3931 cycles for
onset spacing, 3508 for direct KOF gates and 3675 for handoff intervals. Tempo
128/192 and duration 24 retain prior single-track calibration plus owned
component checks here; fresh coupled private checks cover tempo 96 durations
8/16 only. Asynchronous behavior and owned PCM remain separately validated
component behavior, not general original firmware equivalence.

Only sanitized register/timing metadata, hashes and owned PCM statistics are
exported; proprietary assets remain external. Schemas are
`gbb-spc-score-polygate-v1` and `gbb-score-polygate-reference-v1`, retaining
`qualification: false` and `playback: false`. The bundled prototype remains
unchanged. Next is bounded short-articulation support using the other measured
profiles, then broader grammar and separate production integration evidence.
