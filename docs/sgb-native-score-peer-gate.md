# Bounded asynchronous peer gate timing

The opt-in DA diagnostic extends [D9 initial-tick playback](sgb-native-score-initial-tick.md)
with one finite gate calibration. Tempo 96, articulation 127, duration 16 uses
37 timer pulses instead of 36. At 2048 SPC cycles per pulse this adds one pulse
to the timer-driven note gate; it does not change score duration or onset
scheduling. All other gate entries retain their preceding values. No general
duration law or interpolation is introduced.

Fresh direct comparisons exposed eight D9 failures, all the fourth peer note
on SGB1 in retrigger and instrument-switch sequences, across both held voices
and slot mappings. Gates ended 4257..4548.5 SPC cycles early. Every D9 onset
still passed the exact allowance (maximum difference 1429 cycles). The preceding pinned-window
checks allowed a timestamp outside the original window by 4096 cycles; that
could admit a larger difference from the actual original timestamp. The new
checker separately compares each actual onset and gate, retaining exactly
4096 cycles for each difference. It never adds reference-window width.

Qualification and production playback remain false. SGB1/SGB2 program ROMs
remain required. Performance optimization remains deferred.

## Build and identity

`--peer-gate-timing` requires `--initial-score-tick` and all its prerequisites.
The flag must be boolean. The generated 256-KiB image SHA-256 is
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.
DA identifies itself in the host, bridge and recovery paths. SPC payload size
remains 6593 bytes with 63 bytes below the unchanged `$1BFF` cap; the host
remains 1529 bytes. D9, D8, earlier images and the bundled prototype remain
unchanged. Exporters refuse overwrite. No emulator-core or DSP arithmetic
changes are required.

## Direct comparison and lifecycle contracts

The checker uses 32 owned playback scenarios: four asynchronous shapes,
both models, both held voices and both physical slot mappings. It executes
16 fresh original cases with independently authored fixtures and pairs every
note in voice/index order. All 120 notes receive separate onset and gate
comparisons, including the held note and every later peer retrigger.
Original setup, published ENVX trajectories, two-model consistency and held
voice independence must still pass their preceding contracts.

Owned runs retain exact whole-state, observer and PCM reset/save-load checks.
Missing/duplicate cases, reordered/incomplete notes, incorrect identity,
nonfinite timestamps and direct timing differences above 4096 cycles reject.
Public synthetic tests accept exactly +/-4096 and reject another half-cycle.
DA reports use `gbb-sgb-score-peer-gate-v1` and private comparison reports use
`gbb-sgb-score-peer-gate-reference-v1`; D9 controls remain separate and cannot
satisfy a DA reference matrix. Original private traces are temporary and only
bounded register/setup/timing summaries and input hashes are reported.

The complete native child suite is bounded at 3600 seconds and 512 KiB of
stdout. Each probe retains 70 million clocks, 90 seconds, 16 KiB of output
and at most 4096 restores. Original captures retain 8 million instructions,
180 seconds, a temporary trace at most 16 MiB and fewer than 32768 rows.
The full private CTest timeout is 7200 seconds. No partial successful reference
report is emitted on failure.

The dedicated lifecycle suite checks active replacement with/without STOP,
reordered uploads, fragmented cold/active rejection and recovery. An additional
owned playback check covers scalar/native and combined-audio continuation.

## Reproduce

```sh
python3 tests/sgb_score_peer_gate_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe -v
python3 tests/sgb_score_peer_gate_lifecycle_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe -v
python3 scripts/check_sgb_score_initial_tick_reference.py --peer-gate-timing \
  --probe build-dmg-firmware/gameboy_sgb_score_adsr_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
python3 tests/sgb_score_initial_tick_reference_tests.py
```

Original firmware executes with this repository's DSP. These comparisons
qualify bounded firmware control/timing, not DSP arithmetic against independent
hardware or another emulator. Owned samples do not reproduce original timbre
or establish audible fundamental-frequency equivalence. No private instructions,
sample/directory/table bytes, scores or PCM are implementation inputs or
committed artifacts. Acoustic and title qualification remain outstanding.

## Validation evidence

The full fresh checker passes 32 native runs and 16 original executions. All
120 onset and 120 gate comparisons meet the retained 4096-cycle allowance.
Maximum absolute differences, in SPC cycles:

| Shape | Notes | Gate | Onset |
| --- | --- | --- | --- |
| Retriggers | 40 | 3503 | 1395.5 |
| Rests | 24 | 3494 | 911.5 |
| Instrument switches | 40 | 3416.5 | 1459.5 |
| Clipped endings | 16 | 3494.5 | 0 |

The eight previously failing SGB1 fourth-peer gates now differ by at most
2474 cycles. Fresh D9 control measurements still show all eight misses; its
strong full checker rejects them without emitting a partial success report.
DA includes one separate live frozen-D9 control retaining the wider-window
pass and the too-short measured gate. D9's clipped-only behavior is unchanged.

All nine owned playback test methods pass across the full reference child and
a five-method replay/control/build group (79.183 seconds). Their 38 DA scenario
records cover the 32 matrix runs plus six native/scalar/combined records. Each
full-matrix run performs 62..116 exact in-place restores plus cold reset.
The two lifecycle methods pass in 151.412 seconds across ten physical cases.
The initial focused retrigger/switch group also passed in 171.339 seconds.
Four raw initial three-sample release-cadence exceptions remain visible, all
on held instrument 10 in SGB2 rest cases across both held voices and mappings.
The existing D9 phase-profile allowance is unchanged: single-unit drops,
subsequent two-sample cadence, monotonic release and bounded silence remain
mandatory. No observer timestamp or exception counter is corrected.

Seven public comparison/mutation tests and seven asynchronous fixture/parser
tests pass. All five focused CTests pass in 2.47 seconds: comparison guards, asynchronous
fixtures, firmware build/reproducibility and DSP envelopes. The private
registration check passes. D9/D8 hashes and the bundled prototype reproducibility check pass;
`git diff --check` passes. Earlier full physical firmware matrices were not
rerun; their images remain frozen. Independent DSP/hardware, acoustic and
real-title qualification were not performed.

## Next step

The [owned-sample pitch calibration](sgb-owned-sample-pitch.md) now measures
an independently authored periodic pair across the supported octave and
qualifies normal tuning within 10 cents in isolated DSP PCM. Whole-host
upload/replay checks pass; next measure actual whole-host native PCM across
models, voices and slot mappings. Broader instrument banks, score grammar,
echo and production title integration remain separate work.
