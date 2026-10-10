# Subframe SOUND timing during cold recovery

This gate varies actual GB command timing in
[cold mixed one-byte recovery](sgb-bank-cold-mixed-tail-audio.md). One staggered
schedule adds short, quarter-frame, half-frame and nearly full-frame delays
before blocked SOUND, the first fresh start and final stop. The preceding
zero-offset matrix remains a separate gate. This is bounded phase coverage,
not an exhaustive timing sweep.

Firmware, emulator core, DSP, mixer and state format are unchanged. The shared
native probe adds read-only GB cycle/LCD phase observations. All fixtures,
scores, descriptors and samples are independently authored; no proprietary
firmware or assets are implementation inputs or committed artifacts.
Qualification and playback remain false. The replacement program remains
experimental and unbundled; production still requires private SGB1/SGB2
program ROMs. Performance optimization remains deferred.

## Actual GB timing and physical payloads

The cold fixture builders now forward the existing bounded `spin_delays`
argument to the physical cartridge builder. Its established instruction loop
uses `28*n+8` GB cycles for nonzero `n`, with a maximum of 2499 iterations.
The new exporter selects this eight-stage schedule:

| Stage | GB action | Spin count | Added GB cycles |
| --- | --- | --- | --- |
| 1 | Start GB pulse, without SOUND | 0 | 0 |
| 2 | Blocked SOUND start | 1 | 36 |
| 3 | Blocked SOUND stop | 623 | 17452 |
| 4 | Opposite defective SOU_TRN | 0 | 0 |
| 5 | Blocked SOUND start | 1247 | 34924 |
| 6 | Valid SOU_TRN | 0 | 0 |
| 7 | First fresh SOUND start | 2499 | 69980 |
| 8 | SOUND stop and GB mute | 31 | 876 |

The VBlank waits remain 64, 16, 4, 4, 16, 4, 64 and 12. Delay instructions
execute after each stage's VBlank wait and before its HRAM marker/packet.
Nonzero delays may carry execution into the next visible frame. The delay
loop does not itself reach one full 70224-cycle GB frame; later VBlank waits
resynchronize according to their actual execution phase. No time offset is
injected by the probe or audio checker.

All three 4096-byte payloads at cartridge `$4000`, `$5000` and `$6000` remain
byte-for-byte identical to the preceding zero-offset fixture. Both failure
orders retain the one-byte semantic tail and exact preflight defect. Source
controls still differ only in GB routing or fresh BRR data. Checksums are
recomputed, exports are deterministic and existing output files are refused.

## Observed phase and lifecycle contracts

The shared native probe adds `cold-phase`. It records eight rows
`[stage, GB_cycles, physical_scanline, dot]` alongside the existing command
edges, with `command_phase="staggered"` and the actual `phase_model`.
Observations use existing read-only ICD/PPU diagnostics. No observer binding,
core API or save-state field is added.

The checker requires strict integer rows, stages 1–8, increasing bounded GB
cycles, scanlines 0–153 and dots 0–455. Relative GB cycles must agree with
relative host clocks within 32 GB cycles, using SGB1's shared oscillator or
SGB2's dedicated oscillator. For each delayed command, the observed LCD phase
relative to VBlank start must lie between its inserted delay and 128 cycles
after that delay. This allows VBlank polling, IO-marker instructions and the
host instruction observation boundary. The one-iteration delay is also
verified structurally; its short observed band can overlap zero-offset marker
execution. Longer delays cannot pass with an unshifted VBlank marker.

All phase rows must match exactly across source controls and in whole-result
reset/save-load/scalar comparisons. The aggregate checker uses
`gbb-sgb-bank-cold-phase-audio-v1`. All preceding mixed one-byte guards remain:
exact five-token transitions `[1,1,1,3,3]` or `[3,3,3,4,4]`, two rejections,
three suppressions, three clears, one valid publication at generation 2, first
admission before fresh SOUND and one owned source-3 note with final KOF/zero.

Every native right-channel sample before the first note must be zero. Three
isolated controls require whole-stream source isolation and bounded sums
without alignment or gain fitting. Early/late native pitch and seven GB pulse
windows require audible first playback and continuous GB audio through blocked
commands and retry. Clipping and final stereo tails fail.

Every case compares complete host state, PCM, phases and other observations
after cold reset and in-place save/load. Event/unread-output replay masks cover
all eight stages, clears, failures, publication/admission and note
onset/release/zero. Scalar `both` must match complete batched output. Existing
100-million-clock, 1536-restore, one-million-byte PCM, 16-KiB report and
150-second child bounds remain unchanged.

## CI and reproduction

The full staggered matrix has 32 cases: two models, two voices, two orders,
three source profiles and scalar `both`. Each includes reset/restored
execution. `gameboy_sgb_bank_cold_phase_audio_gap_root` and
`gameboy_sgb_bank_cold_phase_audio_root_gap` each retain one complete 16-case
matrix, with 3600-second timeouts and the `sgb-firmware-extended` label. The
four-method public contract stays in platform/sanitizer jobs. The extended set
now has 84 tests across eight dedicated Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_phase_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_phase_audio.py --order root-gap \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_cold_phase_audio_fixture.py --order root-gap \
  --profile native --voice 3 --output /tmp/cold-phase.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_cold_phase_audio_contract$'
```

## Validation evidence

All 32 native cases and eight acoustic comparisons pass, including all eight
scalar controls and exact reset/restored executions. Each capture has 223492
frames, 875–877 restores and 409–410 restores with unread output. Every native
case ran fresh in two concurrent local groups. Final checker revalidation
uses these captures to verify all fixture/PCM hashes and both exact 16-case CI
partitions; it does not rerun native execution.

Observed delayed markers fall 48–72 GB cycles after their inserted loop delay;
the maximum relative GB/host clock discrepancy is 10 cycles. Phase rows match
across controls, scalar, reset and save/load. Compared with the preceding
zero-offset captures, first-note onset moves by 781–802 output frames. This
comparison uses those earlier captures, rather than rerunning their full matrix.

All eight whole-stream source residuals are zero on both channels. The 16
native pitch windows contain at least 20 periods with maximum absolute error
2.821 cents; the 56 GB pulse windows contain at least 19 periods with maximum
error 0.956 cents. Native output stays exactly silent until its first note,
for 157133–160655 frames. Fresh ENVX is 96–97; admission follows publication
by six frames. No samples clip, and both orders retain their exact token
sequences and first valid publication at generation 2.

The four public guard methods pass. They check exact delay instruction counts,
unchanged physical payloads, source-control differences, deterministic
checksummed exports, overwrite refusal, delay bounds, both models/voices/orders,
strict phase identity/rows, wrong clock ratios or LCD phases, missing delays,
stale token/failure state, missing replay and mismatched source phases.

All thirteen focused CTests pass: staggered phase, cold mixed-tail/mixed/tail,
cold, warm mixed/tail, recovery, rejection and replacement contracts, shard
runner and firmware build/reproducibility. The new phase contract passes in
15.21 seconds. CTest discovery assigns the complete `root-gap`/`gap-root`
matrices to dedicated Linux shards 0/7; the public contract stays unlabelled.

Fresh zero-offset SGB1 voice-2 `root-gap` and SGB2 voice-3 `gap-root` regressions
reproduce the preceding mixed one-byte cases' entire metadata and PCM exactly,
including reset/restored executions. All 128 prior cold, cold-tail, cold-mixed
and mixed-tail captures retain their fixture/PCM hashes and pass their
checkers. Those older complete native matrices were not rerun.

Probe build, Python compilation and whitespace checks pass. Private references,
physical hardware and broad title checks were not rerun.

## Evidence limits and next step

Same-renderer owned controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. This gate covers one staggered schedule;
additional phases, semantic defects, banks, clocks and presentation rates
remain separate work. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

Next, cover a complementary subframe schedule that reverses the blocked-command
and fresh-start offsets, retaining observed LCD/GB timing, token, acoustic and
queued-output checks.
