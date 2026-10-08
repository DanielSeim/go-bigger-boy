# Experimental held voice returning directly with a note

The isolated renderer now reproduces the measured direct-note return after
channel 2 has remained keyed through an inactive pattern. It issues KON for
the new note without an intervening old-voice KOF, writes pitch before actual
volume, and gives the returning note its own timer gate. Final readiness then
preserves the measured pending note/rest behavior. The
[original-only observations](sgb-score-direct-return-observations.md) remain
independently reproducible; the preceding
[mixed-articulation rest image](sgb-native-score-final-mixed.md) retains its hash.

## Guarded independent implementation

`build_sgb_score_direct_return.py` composes checked hooks and independently
written `score_direct_return_{shape,pending,voice,on}.asm`. Direct-page C3 marks
the new subtype of the existing BE three-pattern final-return profile. It is
initialized and cleared on a profile miss, so other admitted scores keep their
preceding behavior. The diagnostic checker qualifies only the owned corpus.

The cached guard retains tempo 96, three pattern masks 12/8/12, the initial
16/24-duration clipping geometry, two solo duration-16 events and the bounded
cache terminators. Direct return requires initial articulation 127, leading
channel-2 A0 and channel-3 C9, matching durations 8/16, a pending A1/C9 with
that same duration, and matching articulation 63/127 for all three returning
records. The preceding helper separately checks the first six articulation
records. Final readiness requires the existing channel cursors 82/A2 and
rejects unsupported boundary instrument reselection. Unknown simultaneous
final profiles fail the silent rehearsal. Other completion paths can retain
preceding behavior without claiming direct-return qualification.

Only the guarded third-pattern channel-2 A0 dispatches pitch before volume.
It writes PITCHL/H 164/6, then VOLL/R 7/7. The matching channel-3 rest emits no
pitch, volume or KON. At return, the peer's earlier timer release has already
settled. The guarded onset clears held KOF bookkeeping and writes KOF zero;
the existing onset path writes KOF zero again, then KON 4. Clearing that peer
bit creates no new note or attack. The old channel-2 voice receives no KOF
before the new KON, and its ledger records an unreleased retrigger.

The returning note uses the unchanged measured note profiles:

| Articulation | Duration | Timer pulses |
| --- | --- | --- |
| 63 | 8 | 10 |
| 63 | 16 | 23 |
| 127 | 8 | 15 |
| 127 | 16 | 36 |

These are note profiles, distinct from the preceding mixed-rest nine-pulse
profile. No new interpolation, duration-4 note profile, discarded score pulse,
restarted timer, fitted delay or shifted timestamp is introduced. The old
inactive counter remains frozen until its replacement by the returning note's
profile. All returning notes release and settle before final completion.

The pending final A1 writes pitch 1800 and retains actual volume 7/7 despite
its deferred ED 64. The final controls remain KOF FF/00 followed by KON 4 for
a pending note or KON zero for a pending rest. The final note has no subsequent
timer KOF during the bounded observation window.

The independently reproducible image remains 4089 bytes with SHA-256
`11eba7bf28920d59fd0278de5149263fc6803328554d4c030f8cd2e2f998440a`.
It uses existing gaps within the 4096-byte bound. The preceding image remains
`16f78157cb9c49d4d4b22f7776223c77f81b9fd9aad2a64414892cbd508d882d`.
The bundled prototype is unchanged.

## Physical observations and comparisons

The probe uses schema `gbb-spc-score-direct-return-v1` and exports its explicit
subtype alongside the existing final-return/final-ready flags. Its raw accepted
key-write ledger retains every KOF and KON, including final FF. For this
subtype, physical release edges count currently keyed voices: reasserting KOF
for channel 3 after its earlier release does not invent another note release.
The validator independently folds active voices from the complete raw key
ledger and binds that fold to the physical edges. Previous probe modes and
validator defaults retain their prior contracts.

The old channel-2 envelope has no off timestamp, release steps or fabricated
release-to-zero. Its retrigger timestamp equals the returning KON. The owned
DSP then exhibits a new zero attack, peak 127, decay and a real timer release
that settles to zero. That returning release precedes final termination. The
pending final note starts another attack and remains held; the final rest
stays silent. These are owned DSP/envelope checks, not claims of original
PCM or envelope equivalence.

For each of the eight banks, reset, save/load and cross-engine continuation
preserve full raw events, DSP writes, envelope trajectories and owned PCM.
The probe observes final audio for 600000 half-cycles and restores at four
points in that window. A lost inactive freeze, inserted pre-return release,
wrong pulse profile or reversed pitch/volume order must fail physical checks.
The preceding native image still rejects these banks silently.

All 16 native/original comparisons pass within the unchanged 4096-SPC-cycle
allowance, using the retained independently measured 16-case SGB1/SGB2 corpus:

| Observation | Maximum difference in SPC cycles |
| --- | --- |
| Raw note gate | 2901 |
| Onset interval | 2426 |
| Returning pitch/volume write offset | 445 |
| Returning control write offset | 105 |
| Unreleased-retrigger interval | 255 |
| Final stop interval | 2995 |
| Pending pitch-to-stop interval | 36 |
| Final control write offset | 139 |

The original matrix retains its 2048-cycle intermodel allowance and owned
cartridge-hash checks. The new image also passes all 216 comparisons against
retained pending, reverse, peer, following-rest, ready-final, final-peer,
uniform-return and mixed-rest observations: 232 native/original comparisons
in total. Nine new public physical tests cover the full native matrix, source
faults, silent rejection, raw-write/envelope metadata forgeries and lifecycle
behavior, alongside the preceding eight original-only fixture/parser tests.
All 39 score, fixture and scheduler CTest registrations pass. The complete
build, bundled-image reproducibility check and `git diff --check` also pass.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_direct_return_probe
ctest --test-dir build-dmg-firmware -R 'native_score_direct_return$' --output-on-failure
python3 scripts/check_sgb_score_direct_return_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_direct_return_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-direct-return-playback-reference.json
```

The checker accepts `--reference` for the unchanged sanitized original artifact
and emits schema `gbb-score-direct-return-playback-reference-v1`. The optional
local playback CTest performs the fresh original/native matrix; the separate
original-only registration remains available. Qualification and playback stay
false. Original children retain the 8-million-instruction, 180-second, 16 MiB
CSV and fewer-than-32768-row caps. Native source, program, cache, event, tick,
physical-time, PCM and subprocess bounds remain unchanged. Private original
instructions, scores, instruments, samples and PCM are not inspected or copied.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic outside bundled and
production selection. SGB1/SGB2 program ROMs are still required. Hardware,
original PCM equivalence and real-title firmware replacement remain open.

The subsequent [duration-4 observation corpus](sgb-score-short-return-observations.md)
measures short direct returns on both originals, with pending final events at
independently known durations 8/16. Both returning articulations produce nearly
the same short gate. Native duration-4 gate and envelope qualification remain
the next step; the current diagnostic still rejects these banks silently.
