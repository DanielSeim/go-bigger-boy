# Experimental instrument-2 reselection

The isolated native renderer now executes E0 instrument-2 selections in track
prefixes, including later pattern boundaries. The preceding
[sparse-pattern renderer](sgb-native-score-sparse.md) validated the operand but
discarded the command. Controlled opaque execution with owned score data shows
each selection writing SRCN, ADSR1, ADSR2 and GAIN in that order. The new renderer
resolves its own descriptor and applies all four fields to the selected voice
under KOF, preserving repeated selections instead of coalescing equal values.

This remains an experimental direct-RAM/IPL-trampoline diagnostic outside
bundling and production selection. SGB1/SGB2 program ROMs are still required.
Only instrument 2 is accepted; general instruments, real titles and original
PCM equivalence remain unqualified.

## Prefix setup and timer backlog

`firmware/sgb/score_reselect.asm` counts E0 commands in the track prefix and stores
the count in a parallel `$4A00` cache at the duration/opcode pair offset. It
resets after caching and at each new track. Counts remain bounded by the existing
five-command prefix and 32 executed controls per track. E0 inside a call or
after a timed event rejects. Notes and rests both apply prefix setup; a rest
produces no KON or volume write. Inactive tracks do not execute cached setup.

The independently owned descriptor at `$1540` contains `02/8F/6F/B8`. A field
writer resolves each value and addresses the selected voice's register. Writes
precede event-log publication, note setup and KON. Directory and authored BRR
bytes remain unchanged; `$4A00` now holds instrument counts rather than serving
as a poisoned gap. Source, mask-cache and surrounding guard checks remain.

Longer prefixes exposed a gate issue: pulses accumulated during setup were
charged retroactively to a newly keyed voice. On arming, the renderer reads
timer output and retains those pulses in the existing score-clock queue. Each
new voice excludes remaining pre-KON pulses from its own gate countdown; peer
gates and the fractional score clock still consume them. Cancel, rest,
transition and completion clear the corresponding exclusion state. No pulses
are discarded, timers restarted, timestamps shifted or artificial delays added.
This does not establish arbitrary long-stall handling beyond the retained
four-bit timer-output and diagnostic runtime bounds.

Direct-page `$A4` holds prefix count, `$A5` event count, `$A6` repetitions and
`$A7` DSP base; `$A8/$A9` hold pre-KON exclusions, `$AA` queue scratch and `$AB`
descriptor field index. Other bounds remain: 2048 source bytes, one to four
patterns, channels 2/3 alone or together, eight expanded events per track,
64 events/2032 ticks overall, finite calls and the existing measured pitch,
mix, envelope and ten gate profiles.

`build_sgb_score_reselect.py` composes preceding independently written source
with checked hooks. The strict assembler checks overlaps and operands. The
reproducible 3597-byte image occupies `$0800..160C`, with SHA-256
`879bb92d5633788c11c6afc3c34f575a124b205e2f65a3aa97c216eb43b60277`.
All preceding images and the bundled prototype retain their hashes.

```sh
python3 scripts/build_sgb_score_reselect.py --output /tmp/score-reselect.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_reselect_probe
ctest --test-dir build-dmg-firmware -R 'native_score_reselect$' --output-on-failure
```

## Actual writes and lifecycle

The probe attaches an externally owned CPU's accepted-write observer while
retaining the audio engine's DSP observer and clock binding. It observes actual
F3 writes, resolves F2 read-only, and records physical halves, register, value
and held KOF. Startup writes before live playback are excluded. Observation is
suspended during checkpoint lookahead, avoiding duplicate speculative writes.
Baseline, restored and reset results must match writes, events, DSP edges,
ENVX summaries and owned PCM hashes. Cross-engine continuation compares complete
state and PCM. No production observer API is changed.

Reports expose `instrument_sets` per event and at most 160 `instrument_writes`.
Validation binds each four-field group to its event, voice, prefix count and
pre-event timestamp under KOF. An independent raw scheduler counts instrument
commands. Negative tests omit a same-value DSP write and remove gate-backlog
exclusion; actual write/gate validation rejects both faulty images. Register
snapshots alone would miss an omitted write when its value was already present.

The 37-test suite reruns preceding sparse/list/bank/mix/envelope contracts and
adds all ten gate profiles with one/three/five selections, mask transitions,
solo rest prefixes, the full 64-event/160-write bound, malformed write metadata,
wrong-driver rejection and strict original-observer/comparator guards. All 23
preceding score/fixture/scheduler regression suites also passed, and the bundled
prototype retains its reproducibility hash.

## Original comparison and limits

`build_sgb_reselect_fixture.py` supplies owned 2048-byte, page-crossing
paired/solo-2/solo-3/paired patterns. Eight onsets span ticks 0..112 and end at
128. Initial selection occurs on both voices; later active tracks select
instrument 2 once (`reselect-1`) or twice (`reselect-2`) before pan, track volume
and two notes. Both articulations and both original models are required. The
observer pins actual KON masks, voice/pitch/mix/setup, and later E0 write groups.
It requires directly observed KOF rather than inferring release from a handoff.

```sh
python3 scripts/check_sgb_score_reselect_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_reselect_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Eight fresh opaque original runs passed, with 96 directly observed per-voice
releases. Native/original gate duration differs by at most 4060 SPC cycles,
onset spacing by 4025, and original model onset spacing by 48. The existing
4096-cycle native/original, 2048-cycle inter-model and 84000..92000-cycle original
onset bounds remain unchanged.

| Case | Articulation | Owned cartridge SHA-256 |
| --- | --- | --- |
| reselect-1 | 127 | `38df4b6612c85d8bc922a8007b92bbb8760147b6d739f847aa85ff585fdaaed4` |
| reselect-1 | 63 | `4e6cff0244b27272f3fa99a357911142a8025870b599a0890c5beaf1744bbd7a` |
| reselect-2 | 127 | `6464df708efefff3690de8c0812020e376d61fa63cdc93d062bc06bcd406631e` |
| reselect-2 | 63 | `beaecdc92f893b1aeecb3e5aa355519d4d7f53df9c03bae399870448b6a81d81` |

A separate three-selection diagnostic on SGB1 at articulation 127 produced an
original onset interval of 92311 SPC cycles, outside the retained fixture
contract. It is excluded from reference qualification. Native lifecycle and
physical gate tests cover prefixes through five commands; original longer-prefix
timing remains open. This qualifies the measured one/two-selection profile,
not general command-prefix timing or other instruments.

Private firmware remains opaque execution-only input: no original instructions,
samples, scores or instrument tables are inspected or committed. Child runs
retain 8000000-instruction/180-second bounds, a 16-MiB temporary trace limit and
fewer than 32768 rows. Only sanitized DSP metadata, timing and hashes are
exported. Schemas are `gbb-spc-score-reselect-v1` and
`gbb-score-reselect-reference-v1`, with qualification and playback false.
