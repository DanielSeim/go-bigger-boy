# Experimental trailing mix controls and end priority

The subsequent [clipped-peer lifecycle diagnostic](sgb-native-score-peer.md)
resolves the measured release/retrigger gap and inactive gate behavior.

The isolated native renderer now accepts trailing E1 pan and ED track-volume
commands after a track's final timed event. Opaque runs on both originals show
that only the track that ends the pattern executes these commands. Channel 2
wins simultaneous endings: its controls execute, while channel 3's are skipped.
The preceding [timing-inheritance diagnostic](sgb-native-score-timing.md)
continues to build independently with its original hash and rejection rules.

## Bounded implementation

At each expanded track's zero terminator, the parser caches its final pan and
volume fields in the existing parallel cache pages. No source or guard region
is repurposed. Eight-event tracks use terminator offset 16 within their 32-byte
slot, including the final slot at offset 240.

Before advancing a pattern, `tail_end` selects the ending channel's terminator.
The retained scheduler rejects ambiguous end/note ordering, so a zero channel-2
countdown identifies its end; otherwise channel 3 ends. This also selects the
correct active channel in a solo pattern. Resolving these fields through the
existing rest path updates inherited pan/volume without VOL, KON, instrument
writes or event-log records. Inactive and clipped peer controls do not execute.
Rehearsal follows the same path and validates any later note's inherited mix
before publishing live audio. A trailing combination can therefore be held
through a rest, but an unmeasured sounding combination still rejects.

The independently written symbolic scheduler adds `end_priority=True`, stopping
track processing immediately at the first end in channel order. Its default
retains the preceding behavior. The new checker explicitly requests priority
and timing inheritance. Same-tick end/note cases remain rejected by the native
renderer and are not qualified by these fixtures. Trailing instrument, tempo,
song-volume and incomplete timing commands remain unsupported.

Other bounds are unchanged: 2048 source bytes, four patterns, eight expanded
events per track, 64 events, 2032 ticks, finite calls and the existing ten gate
profiles and measured source/envelope/mix points. The reproducible 4016-byte
image ends at `$17AF` and has SHA-256
`f5455964ec1246b4cf6798d0d38c812cdd26dad87924029a3341ac1b3e70caf2`.
`build_sgb_score_tail.py` composes checked hooks with the owned helper at `$1790`.
The strict assembler checks placement and operands. Previous images, sample
bytes and the bundled prototype are unchanged.

```sh
python3 scripts/build_sgb_score_tail.py --output /tmp/score-tail.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_tail_probe
ctest --test-dir build-dmg-firmware -R 'native_score_tail$' --output-on-failure
```

## Complete-ending reference checks

All owned fixtures have eight onsets, masks `12,4,8,12`, and starts at ticks
`0,32,64,96`. Instrument 2, centered pan, track volume 127, song volume 160 and
tempo 96 are explicit at startup. Four cases qualify complete paired endings:

- `tail-2` ends channel 2 with ED 64; its later tracks resume the measured `1/1`
  DSP pair. Channel 3 remains `7/7`.
- `tail-3` ends channel 3 with ED 64 at the same tick as channel 2; the command
  is skipped and both channels remain `7/7`.
- `pan-2` ends channel 2 with E1 20; its later tracks resume `11/0`.
- `pan-3` ends channel 3 with E1 0 at the same tick as channel 2; it is skipped.

Sixteen fresh executions cover these cases, articulations 63/127 and both
SGB/SGB2 originals. All 192 per-voice KOF releases are directly observed. Native
pitch, volume, source, envelope, ordering, pattern entries and gate/onset timing
match the retained contracts. Maximum native/original gate difference is 3462
SPC cycles; maximum onset interval difference is 2671. The existing 4096-cycle
allowances, 84000..92000 original onset bounds and 2048-cycle intermodel allowance
remain unchanged. Maximum intermodel onset difference across all six fixture
cases is 56 cycles. No timestamps or timer phase are shifted and no artificial
delays are added.

```sh
python3 scripts/check_sgb_score_tail_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_tail_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms --articulation 127 > /tmp/score-tail-127.json
# Repeat with --articulation 63.
```

The checker can omit `--probe` for original-only measurement. Each child retains
8 million SNES instructions, a 180-second timeout, a 16 MiB CSV bound and fewer
than 32768 rows. Reports export sanitized DSP/timing metadata and input hashes;
private instructions, scores, instrument tables, samples and PCM are not copied.
Local results require caller-owned originals and do not establish PCM equivalence.

## Clipped-peer release gap

Eight further fresh original runs measure `clip-2` and `clip-3`, where the chosen
channel ends at tick 32 and its peer's last note has duration 24 rather than 16.
Both tracks contain trailing ED 64. The ending track's ED executes, while the
peer's unplayed command does not. This confirms the control priority under
first-end clipping, but it exposes a separate voice-lifecycle mismatch:

- At articulation 63, the peer's measured short gate releases before the
  boundary, and all 12 releases are observed.
- At articulation 127, the original does not publish KOF for the peer's second
  note before its next KON. `clip-2` retriggers channel 3 at onset 4; `clip-3`
  retriggers channel 2 at onset 2. Each original therefore has 11 observed
  releases and one explicitly recorded unreleased retrigger.

The observer records a missing release as an unreleased retrigger, never as an
inferred KOF. The diagnostic contract pins voice, onset indices, articulation,
remaining real releases and actual onset metadata. These clipping runs are
excluded from native gate qualification: the current native renderer forces a
peer release at pattern changes. Native clipped-tail tests verify control state
only. Do not treat their passing results as proof of original voice-lifecycle
compatibility.

The 13-test suite checks complete-ending fixtures, solo endings, simultaneous
priority, skipped clipped controls, rests and unmeasured inherited combinations,
call return/repetition, all ten gate profiles with full cache slots, retained
rejections, an omitted-boundary-helper fault, opt-in symbolic priority and strict
reference/retrigger metadata mutations. The shared physical probe also checks
actual DSP edges and writes, ENVX, owned PCM, reset, save/load and cross-engine
continuation, source preservation and cache guards. All 13 new tests and the
26 preceding score/fixture/scheduler suites passed (27 CTest registrations),
as did the bundled-image reproducibility check and `git diff --check`.

Schemas are `gbb-spc-score-tail-v1` and `gbb-score-tail-reference-v1`. This remains
an opt-in diagnostic with qualification and playback false, outside bundled and
production selection. SGB1/SGB2 program ROMs remain required. The subsequent diagnostic qualifies the measured clipped-peer release and
retrigger behavior, including inactive countdowns and rest reactivation.
