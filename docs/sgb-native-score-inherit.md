# Experimental per-channel mix inheritance

The subsequent [timing-inheritance diagnostic](sgb-native-score-timing.md)
retains duration and articulation across patterns and inactive channels.

The isolated native renderer now retains E1 pan and ED track volume between
patterns. Owned fixtures executed against both opaque originals confirm that
these controls survive a channel's inactive pattern as well. A control changes
only its channel and its field. Explicit later controls still override it.
The preceding [instrument reselection diagnostic](sgb-native-score-reselect.md)
continues to build independently with its original behavior and hash.

## Execution order and limits

Parsing a whole track before playback can encounter controls in an unplayed
tail: the other track may end the pattern first. Carrying the parser's final
values would apply those controls incorrectly. Instead, each cached pan/volume
field is `$FF` until that track explicitly supplies it. The playback scheduler
resolves cached controls against separate channel-2/3 state when it executes a
note or rest. Thus inactive tracks and unplayed tails cannot change inherited
state. Calls and repeated bodies use the same expanded-event path.

Direct-page `$AC/$AD` contain pan and `$AE/$AF` track volume. Each complete
scheduler run initializes these fields to center/127. Initialization runs for
both the silent rehearsal and live replay, rather than at every pattern.
Rehearsal resolves and validates all executed inherited combinations before
publishing DSP setup, instrument writes, audio or events. Rests can update mix
state without writing volumes or keying on; a later note must still select an
already measured mix point. Unmeasured endpoint reductions and combined song
and track reduction reject before live playback.

Controls after a track's final timed event are not qualified by this milestone.
The parser uses `$B0` to reject these trailing commands. Commands at a callee's
end remain usable when a later caller/body timed event consumes them. This is
an explicit restriction rather than a guessed boundary-ordering rule. Native
clipped-tail tests verify scheduler order; opaque-original qualification below
covers equal-length paired patterns and solo/reactivation transitions, not
arbitrary first-end clipping or trailing commands.

Source, event/cache guards, actual DSP write observation, ENVX behavior, owned
PCM checks, reset and state restoration retain the preceding probe contracts.
The source bank remains bounded to 2048 bytes, the program to 4096 bytes, and
playback to four patterns, eight expanded events per track, 64 events and 2032
score ticks. The existing ten tempo/articulation/duration gate profiles apply.
Duration and articulation still require explicit setup in each active track.

`build_sgb_score_inherit.py` composes independently written source with checked
hooks and the strict assembler. The reproducible 3797-byte image occupies
`$0800..16D4`, with SHA-256
`4808a273bbad7aeacec7174038b33e37b7cc615e8c3303a844a07bf8fa19ef15`.
The owned source/envelope descriptor, BRR bytes and bundled prototype remain
unchanged. The dedicated probe reuses the existing actual-write/lifecycle
implementation and emits `gbb-spc-score-inherit-v1`.

```sh
python3 scripts/build_sgb_score_inherit.py --output /tmp/score-inherit.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_inherit_probe
ctest --test-dir build-dmg-firmware -R 'native_score_inherit$' --output-on-failure
```

## Opaque reference evidence

`build_sgb_inherit_fixture.py` supplies two owned 2048-byte banks. Each has eight
onsets with pattern masks `12,4,8,12` and starts at ticks `0,32,64,96`:

- `pan-hold` starts channel 2 fully left and channel 3 fully right. Later solo
  tracks omit E1; channel 2 explicitly returns to center in the final pair,
  while channel 3 resumes its earlier right pan.
- `track-hold` starts both centered with ED 64. Later solo tracks omit ED;
  channel 2 explicitly restores 127 in the final pair, while channel 3 resumes
  64. With song volume 160, measured DSP pairs are respectively `1/1` and `7/7`.

Both fixtures explicitly select instrument 2, tempo 96 and song volume 160 at
startup. Articulations 63 and 127 produce four cartridges:

| Case | Articulation | Cartridge SHA-256 |
| --- | --- | --- |
| pan-hold | 127 | `4e33dcd86247ea9e0fcb094fe1cced7da75a5f7eb4171c6e81134e4451b07c3f` |
| pan-hold | 63 | `a0ae475ece7fa11f1ab7ae9dff63dded5d6b0b897ca8f11dedca78b5b05a1aa8` |
| track-hold | 127 | `e9132dd8ec200bacb69c3d082ab2edb922ca2d1bb181db507bed6b8ee0a3ef63` |
| track-hold | 63 | `68906e8cbcc43fbbd40a6cde28d523446ed45047cba6a501eda57d150b315119` |

Eight fresh original executions cover these cases, articulations and both
SGB/SGB2 models. Actual pitch, volume, source and envelope metadata match the
owned contract. All 96 per-voice releases are directly observed KOF edges.
Native/original maximum gate difference is 3462 SPC cycles; maximum onset
interval difference is 3019. The existing 4096-cycle allowances and original
84000..92000 onset bounds remain unchanged. Maximum intermodel onset difference
is 56 cycles, below the retained 2048-cycle allowance. No timestamps, timer
phase or artificial delays are adjusted to fit these results.

```sh
python3 scripts/check_sgb_score_inherit_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_inherit_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-inherit-reference.json
```

The checker also accepts omission of `--probe` for bounded original-only
measurement. Each original execution retains the eight-million-instruction,
180-second child, 16 MiB trace and 32768-row bounds. Reports export sanitized
register/timing observations and input hashes; no private code, scores,
instrument tables, samples or PCM are copied. Private inputs and these local
checks are not reproducible without the caller's originals. This evidence does
not establish PCM equivalence with them.

The 14-test native suite covers carry, partial overrides, inactive reactivation,
clipped tails, rests, call return/repetition, all ten gate profiles with actual
prefix writes, 64-event save-count carry, silent rejection, a live-initialization
fault and reference metadata/timing mutations. The shared probe checks baseline,
restored, cold-reset and cross-engine continuation, actual DSP edges and writes,
ENVX and owned PCM throughout these runs. All 14 tests and the 24 preceding
score/fixture/scheduler suites passed, as did the bundled-image reproducibility
check and `git diff --check`.

This remains an opt-in diagnostic, with qualification and playback false. It is
not bundled or selected by production playback. SGB1/SGB2 program ROMs remain
required. Duration/articulation inheritance is covered by the subsequent diagnostic.
Trailing-control boundary ordering needs a separate owned reference experiment
before it can be accepted.
