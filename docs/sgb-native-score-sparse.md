# Experimental solo tracks and sparse patterns

The isolated native renderer now accepts channel 2 alone, channel 3 alone, or
both in any of its one to four patterns. Zero pointers in those channel slots
mean absent tracks; a nonzero pointer must still resolve to a nonempty valid
stream. Completely empty patterns and other populated channel indices reject
before live events or audio. This extends
[bounded phrase lists](sgb-native-score-list.md) while retaining their 2048-byte
source, eight-event track, 64-event phrase and 2032-tick bounds.

The driver remains an experimental direct-RAM/IPL-trampoline diagnostic outside
bundling and production selection. SGB1/SGB2 program ROMs are still required.
This does not qualify real-title playback or replacement whole-system upload.

## Active tracks and ownership

The parser reserves both 32-byte track slots in every pattern, including absent
tracks. It clears the absent slot's duration sentinel without following a zero
pointer. Each pattern gets a cached active mask, 4/8/12, at `$4B00+index`.
The parser mask is direct-page `$A2`; the current runtime mask is `$A3`.

The scheduler loads and decrements only active tracks. A solo track advances the
phrase when it ends; paired tracks retain first-end clipping and rejection of
ambiguous same-tick end/peer events. Every pattern transition releases both
outgoing voices and cancels their pending gates. Only the new active tracks
produce events, pitch/volume writes and KON. Absent voices stay in KOF with zero
gate counters; any outgoing release tail must settle before the probe's
20000-half-cycle inactive-envelope check. Inactive voices may have a natural
release tail, rather than becoming abruptly silent at the boundary.

The timer now starts after the initial pattern's release wait and voice
preparation, immediately before its first `poly_on`. It also starts for a
rest-only initial pattern. No later pattern restarts the timer or fractional
score clock. This prevents startup work from consuming the first note's timer
phase. Gate profiles, physical timing allowances, event timestamps and runtime/
audio bounds remain unchanged.

Shared E5 song volume may be selected once in the first active track's prefix
of the first pattern. This allows a channel-3-only phrase to select volume; it
still rejects selection on channel 3 when channel 2 is populated, on later
patterns, inside calls, or after timed events. Duration/articulation begin unset
per track, pan/track volume reset to 10/127 per track/pattern, and shared song
volume defaults to the existing diagnostic value 160 and persists. Original
implicit defaults are not qualified by these fixtures; they select volume
explicitly. All preceding measured note, mix, call and envelope restrictions
remain.

The memory layout is unchanged through the `$4900` song-volume cache. `$4A00`
is a poisoned gap, `$4B00..4B03` hold the active masks, and `$4B04..4CFF` are
poisoned guards. The real component requires those guards and the source pool
to survive rendering, quiet tail, reset and restore. The two-page committed
event log and its count-carry restore checkpoints remain in use.

`score_sparse.asm` supplies the independent parser, and
`score_sparse_runtime.asm` supplies solo scheduling and initial timer setup.
`build_sgb_score_sparse.py` composes the preceding source with checked hooks.
The strict assembler checks overlaps and operands. The reproducible 3345-byte
image occupies `$0800..1510`, with SHA-256
`a95e890efb333306c0c87b17e742eaee51178eab498b1973ddc436774c37a187`.
Earlier images, directory/sample bytes and the bundled prototype remain unchanged.

```sh
python3 scripts/build_sgb_score_sparse.py --output /tmp/score-sparse.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_sparse_probe
ctest --test-dir build-dmg-firmware -R 'native_score_sparse$' --output-on-failure
```

## Validation profile

The 31 ROM-free tests rerun preceding list/bank/mix/envelope contracts, updating
zero-pointer assertions to distinguish an absent track from a malformed active
stream. New tests cover a single event on either voice under all ten gate
profiles, rest-only solo phrases at 2032 ticks, both stereo endpoints, all nine
ordered active-mask transitions, paired clipping into solo playback, shared
song-volume selection, calls/repeats/return inheritance, page-crossing reads and
typed mask/reference guards. Empty, fifth, truncated, unsupported-channel and
malformed later-pattern inputs reject silently.

The symbolic scheduler now supports channel 3 alone as well as channel 2 and
paired patterns. It independently checks events, controls, pattern starts and
active masks. Its own tests ensure there is no synthetic peer on a solo track.
The physical APU probe checks inactive KOF/KON, pulse counters and settled ENVX
on every half cycle, with whole-engine/cross-engine restore and reset reproducing
events, DSP edges, ADSR summaries and owned PCM hashes. The full 64-event carry
tests remain covered. No original PCM equivalence is claimed.

`build_sgb_sparse_fixture.py` supplies three owned 2048-byte cartridges:

- `solo-2`: eight channel-2 onsets, including three executions of a shared body,
  dynamic ED volume and inherited return settings.
- `solo-3`: the same sequence entirely on channel 3, with shared volume selected
  in that channel's initial prefix.
- `transitions`: eight onsets through paired/solo-2/solo-3/paired patterns at
  ticks 0/32/64/96, ending at 128. Instrument 2 is selected initially on both
  voices; later patterns explicitly set pan and track volume.

```sh
python3 scripts/check_sgb_score_sparse_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_sparse_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The strict original observer checks the actual KON mask and only its selected
voices, binds KOF releases to those voices, and never infers a missing release
from a handoff. The preceding shared observer was corrected to honor KON bits
instead of inventing a peer note for every onset. Both original models and both
articulations are required for every case.

Twelve fresh opaque original runs passed, with all 112 per-voice releases
directly observed. Native/original gate duration differs by at most 3579 SPC
cycles, onset spacing by 3611, and inter-model onset spacing by 56. The existing
4096-cycle native/original and 2048-cycle inter-model allowances remain unchanged,
as do the 84000..92000-cycle original onset bounds. The 31-test sparse suite,
twenty-two preceding score/fixture/scheduler suites and bundled reproducibility
check passed. After the transition fixture stopped redundantly reselecting
instrument 2, its two fixture/observer tests were rerun and passed.

| Case | Articulation | Owned cartridge SHA-256 |
| --- | --- | --- |
| solo-2 | 127 | `1c479a34723f0ea2d72c635a99503fabae8dadc4631c8094c8184eac43ab0ab1` |
| solo-2 | 63 | `6b8271d6f0fb016b27b2191780d949ecf1abe1f235d5ea81be8742688411336f` |
| solo-3 | 127 | `c62ff7d465c7aecca4a93030dca01e0f51db352f27a46ccd0c2c0b98a2ef1644` |
| solo-3 | 63 | `7adfa154349d760ffbafbd833ddd63a256924c8ba9f0b518e7383d920eeaac8b` |
| transitions | 127 | `d98d549e243f57da63e93613ea1654544e37401df18cf2ffc1b7510bb557318b` |
| transitions | 63 | `a43ecac85c69548f761fc3265daef641b9c237c6b8b30c4f48b178f4d19d1dba` |

Redundant E0 instrument-2 selection on later patterns remains outside the
timing qualification. A diagnostic variant with those additional commands had
a 4224-cycle onset discrepancy, beyond the unchanged 4096-cycle allowance.
The qualified transition profile retains initial instrument selection and later
pan/track controls, matching the preceding qualified fixtures. This limitation
must be resolved before claiming general prefix-command timing or instrument
transition compatibility.

Private firmware remains opaque execution-only input. No original instructions,
samples, scores or instrument tables are inspected or committed. Child runs
retain their 8000000-instruction/180-second bounds, 16-MiB temporary trace limit
and fewer than 32768 rows; only sanitized DSP/timing metadata and hashes are
exported. Schemas are `gbb-spc-score-sparse-v1` and
`gbb-score-sparse-reference-v1`, with qualification and playback false.
