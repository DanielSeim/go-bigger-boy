# D9 initial score tick and clipped song-end timing

The opt-in D9 diagnostic fixes the late clipped KOF identified by the
[D8 asynchronous checks](sgb-native-asynchronous-adsr.md). The initial prepared
note occupies the first logical score tick. D9 consumes that tick synchronously
after initial onset, instead of waiting for the first fractional-clock dispatch
before starting its countdown. The original logical end tick remains unchanged.
The initial countdown phase changes; timer-driven audio gates retain their
preceding counters, lookup table and timer path.

Fresh private-original comparisons put the corrected held KOF within 362 SPC
cycles of the original, on both models, voice orientations and physical slot
maps. The retained reference allowance is 4096 cycles. Qualification and
production playback remain false; SGB program ROMs remain required. D8 and
earlier images remain frozen, and performance optimization remains deferred.

## Implementation and bounds

`build_sgb_score_transport.py --initial-score-tick` requires D8's
`--instrument-envelope` and its existing prerequisites. Both flags are strictly
boolean. Only this variant advertises D9 in the bridge, recovery path and five
host comparisons. Defaults still build their preceding images.

The independently authored `score_initial_tick.asm` helper prepares the initial
real pattern through the existing `multi_begin`, increments the public logical
tick once and tail-calls `pair_tick`. It substitutes a call target of the same
size in the existing engine, preserving fixed `.org` placements. Its eight
bytes append after the preceding helpers. The silent whole-bank rehearsal,
source bytes, expanded caches, later pattern transitions and timer-gate pulses
keep their existing paths. Qualified initial durations exceed one tick, so the
initial dispatch cannot finish a track at startup.

Every real selection enters this path once, including reselection and playback
after upload/recovery. It does not run in the directory's silent validation or
again at subsequent pattern boundaries. STOP, atomic invalidation and recoverable
rejection retain their preceding contracts.

The 256-KiB image SHA-256 is
`b8d12f0632cb1893ff1518fbbbeb225c0077b80b7ab1fd93f72e8d7ff331939f`.
The SPC payload is 6593 bytes (`$0200..1BC0`), leaving 63 bytes under the unchanged
`$1BFF` cap. The host remains 1529 bytes (`$8000..85F8`). D8 retains
`8cb99b32c080368a0573a7a0c125522e9d5940ef0a4cd59a51ab9971993c47c0`;
D7 and all earlier checked image hashes also remain unchanged. Exporters refuse
overwrite. No emulator core, DSP arithmetic or production path changes.

## Playback and lifecycle qualification

D9 reuses the owned asynchronous fixtures and the same read-only ADSR observer.
The four cases remain peer retriggers, inserted rests, alternating instrument
10/2 selections and early final-song clipping. Both voice orientations and both
physical slot maps execute on SGB1/SGB2. Full cases finish at logical tick 80;
the clipping case at tick 32. The preceding D8 envelope, descriptor/source/pitch,
setup stability, observed KOF, zero-tail and exact state/owned-PCM replay checks
remain in force. All D9 native gate and onset observations must also pass the
existing original-reference windows with the unchanged 4096-cycle allowance.
The 180000-cycle observation ceiling in the D8 clipping diagnostic does not
substitute for that reference comparison.

Native/scalar/combined switch runs preserve the preceding mode contracts: exact
native/scalar PCM, identical physical envelope observations in all modes and
the combined output-frame ratio. Two frozen-D8 controls still reproduce the
late clipped release; they are reported separately from D9's timing result.

D9 also reuses physical replacement with/without STOP, reordered active reupload,
fragmented cold rejection/recovery and repeated active rejection/STOP/recovery
cases on both models. These require D9 identification, unchanged logical end
ticks, descriptor/tuning/map and region hashes, expected interruption and atomic
clearing evidence, silent rejection, and exact reset/save-load/owned-PCM replay.

Published ENVX still depends on the DSP/KOF latch phase. The preceding finite
initial three-sample inter-drop exception remains allowed for instrument-2 switch
notes. D9 additionally observes that same initial transition on the held
instrument-10 voice in the SGB2 rest-case, held-voice-2 reversed-slot arrangement. The raw
exception counter and eight-sample release prefix are preserved. Exactly one
initial three-sample interval may occur in these calibrated profile/case families;
one-unit drops, two-sample intervals afterward, no later exception, monotonic
release, bounded zero time and full replay remain mandatory. D8's allowed phase
profiles remain unchanged. No timestamp or counter is corrected in the observer.

The owned-suite schema is `gbb-sgb-score-initial-tick-v1`. It separates D9 runs
from frozen-D8 controls and retains raw per-note timing, reference windows and
individual match flags. Its timing result concerns those measured windows;
general qualification/playback remain false.

## Fresh exact original comparisons

`check_sgb_score_initial_tick_reference.py` runs the eight D9 clipped
model/voice/slot cases, then executes four newly authored clipped fixtures on
the private originals. Original source/pitch/envelope setup, complete published
ENVX windows and trajectory contracts are checked afresh. It then compares each
actual native gate directly to its corresponding original gate, without adding
the width of a reference window to the allowance. All sixteen comparisons must
remain within 4096 SPC cycles.

The native held gates are 170538..170625.5 cycles; the originals are
170746..170907. Maximum absolute held-gate difference is 362 cycles. Including
the peer's ordinary timer-driven KOF, maximum difference is 1420.5 cycles.
This fixes the preceding held-note miss of roughly 5800 cycles without expanding
the timing allowance. Half-cycle precision is retained in native observations.

The checker accepts only a successful, bounded, complete eight-run native
report for the pinned D9 image. It rejects missing/duplicate cases, wrong versions
or image hashes, incorrect reset/save-load metadata, invalid numeric types,
changed original envelope summaries and direct gate differences over 4096.
Native execution retains 70 million clocks, 90 seconds per probe, 16-KiB probe
output and 4096 restores; the eight-run child suite has a 900-second bound and
128-KiB report cap. Original execution retains 8 million instructions, 180
seconds per child, a 16-MiB temporary trace and fewer than 32768 rows. Only
instruction-bound original completion is accepted. No partial report is emitted.

The reference schema is `gbb-sgb-score-initial-tick-reference-v1`. Only bounded
setup/trajectory summaries, timing comparisons and input hashes are retained.
Raw private traces and child stdout/stderr remain temporary. Originals execute
with this repository's DSP; these measurements independently check firmware
control/setup, not DSP arithmetic against hardware or a second emulator. No
private instructions, instrument/sample/directory bytes, scores or PCM are
implementation inputs or committed artifacts. Acoustic and title qualification
remain outstanding.

## Reproduce

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --atomic-upload --upload-recovery --instrument-mapping \
  --instrument-tuning --instrument-envelope --initial-score-tick \
  --output /tmp/d9-host.rom
python3 tests/sgb_score_initial_tick_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe -v
python3 tests/sgb_score_initial_tick_lifecycle_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe -v
python3 scripts/check_sgb_score_initial_tick_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'sgb_score_initial_tick'
```

Private CTest registration requires all caller-owned firmware inputs. Public
comparison/parser mutation tests and build checks need no private images; the
owned playback and lifecycle suites use the probe and independently authored
firmware/samples only.

## Validation evidence

All nine owned-suite methods were validated, with a focused rest-method rerun
after calibrating its initial published latch transition (78.588 seconds).
The final 38 D9 scenario records contain 150 per-note observations, all passing
the retained reference-window gate/onset allowance, with 62..115 exact in-place
restores per run plus cold-reset replay. The two frozen-D8 controls retain their
clipped timing misses. The two D9 lifecycle methods cover ten physical cases
and pass in 160.179 seconds.

The fresh private checker passes eight additional D9 clipped probe runs and
four original executions, including all sixteen direct gate comparisons.
Three public comparison/mutation tests pass. Thirteen preceding build/cap/
reproducibility methods pass in 6.164 seconds; eleven focused regression CTests
pass in 106.13 seconds. The bundled prototype reproducibility check and
`git diff --check` pass. Earlier full D8 physical matrices were not rerun;
their image hashes and frozen clipped controls remain checked.

## Next step

Extend fresh direct original/native timing comparisons to the remaining
asynchronous retrigger, rest and instrument-switch cases. The current whole-host
suite checks their pinned original windows; only clipping has fresh direct
timestamp comparisons in this milestone. Keep the 4096-cycle allowance, compare
each peer gate/onset independently and address any remaining misses before
expanding instrument/sample coverage or production integration.
