# Experimental final-boundary note and rest execution

The independent diagnostic now executes a ready channel-2 note/rest before a
final channel-3 end with no following pattern. Both originals issue KOF FF,
KOF 00 and then the accumulated KON mask. A note receives a last channel-2 KON
and remains without a later KOF during the bounded observation; a rest produces
KON 00 and silence. Final completion therefore cannot always force silence.
This extends the [following-rest diagnostic](sgb-native-score-follow-rest.md)
in a new image, preserving earlier independently buildable images and guards.

## Owned measurements

`build_sgb_score_final_fixture.py` terminates the phrase list after its first
owned pattern. Both voices initially select instrument 2, centered pan, track
volume 127, song volume 160 and tempo 96, then play duration-16 notes 98/99.
At tick 32, channel 2 executes ED64, duration 8/16, articulation 63/127 and note
A0 or rest C9 before channel 3 ends with ED64. Eight distinct owned 2048-byte
banks cross the boundary kinds, durations and articulations. Each runs on SGB1
and SGB2: sixteen fresh original measurements.

Both cases have paired KON at ticks 0/16, pitches 1068/1132 and volume 7/7.
All four preceding notes receive their normal measured timer releases before
the final boundary. A final note additionally writes channel-2 pitch 1700 and
receives KON 04 at tick 32, retaining volume 7/7. A final rest writes no pitch
and receives no new note KON. Neither case applies logical ED64 to DSP volume:
no voice-2/3 volume writes occur after the second onset.

The observed final key-register sequence is:

| Register | Value | Offset from final KOF FF, SPC cycles |
| --- | --- | ---: |
| KOF | FF | 0 |
| KOF | 00 | 117 |
| KON | 04 for a note, 00 for a rest | 156 |

The note's final pitch write precedes KOF FF by 464 cycles. There are 64 real
preceding-note releases across the matrix, eight newly keyed terminal notes,
and no unreleased retriggers. The terminal notes have no later KOF throughout
the fixed instruction runs. Post-stop trace windows span 2955715..4707996 SPC
cycles across the cases and models. These are bounded observations, not a claim
that a voice sounds forever: ADSR can decay without a KOF. The two models have
different remaining wall times under the same instruction cap, so these window
lengths are not compared as gate durations.

Maximum intermodel differences are 49 cycles for preceding gates, 48 for onset
and stop intervals, and zero for pitch-to-stop and final control-write offsets.
The retained intermodel allowance is 2048 cycles.

## Native execution and scope

`scripts/build_sgb_score_final.py` composes checked hooks over the preceding
renderer and independently authored `firmware/sgb/score_final*.asm` source.
The final-ready guard requires tempo 96, a single pattern, exactly two prior
cached events on each track, and duration 16 for both second events. Existing
silent rehearsal also requires a measured duration-8/16 boundary event and
rejects a boundary instrument prefix before live playback. These restrictions
keep the newly admitted case within the measured preceding-release lifecycle;
multiple-pattern final boundaries and already sounding peers remain outside it.

The guard suppresses boundary volume writes while retaining its logical controls
and pending KON bit. Completion clears gate processing, writes KOF FF/00, then
issues the pending KON mask. The note has no armed timer gate after score
completion. Before clearing KON, the runtime polls the owned DSP ENVX register
until attack becomes visible. This functional handoff preserves KON across the
DSP sample latch; it uses no calibrated delay. A rest skips that attack wait.
Ordinary completion continues to use the preceding release behavior.

The new private DP mode is reset at each replay. The score ends at tick 32 while
the final raw record also has tick 32 and still belongs to the old pattern. The
new validator explicitly admits only a last channel-2 record at the terminal
tick; preceding validators retain their strict before-end bound. No event is
removed, shifted or rewritten to fit a physical projection.

The image is 4059 bytes. SHA-256:
`00ff6c59ecdb5915b9cb2e742b45b17bb1422438150923c0be2cc6877d7ddf7b`.
The existing 4096-byte program, 2048-byte source, four-pattern,
eight-events-per-track, 64-event, 2032-tick, 30-million-half-cycle,
500000-PCM-frame and 60-second native-child bounds are unchanged.

## Validation and reproduction

The final probe records actual accepted KON/KOF writes as well as pitch/volume
writes. It binds each edge to a register write and checks the stop pulse even
when all preceding notes already released. A distinct terminal observation runs
for 600000 physical half cycles (300000 SPC cycles), longer than every admitted
gate profile and within the existing physical/PCM caps. The note must retain
positive ENVX and audible PCM; the rest must retain zero ENVX and silent PCM.
The held trajectory has no invented KOF, release steps or retrigger timestamp.
Quiet-completion requirements remain unchanged for other diagnostics.

Reset and same/cross-engine save/load checks include the full terminal PCM
fingerprint and snapshots throughout this post-score window, in addition to
actual pitch-write checkpoints and earlier lifecycle stages. Source/cache guards,
logical controls, symbolic execution order, accepted DSP writes, owned ADSR,
physical gate identities and cause, terminal-state metadata and byte/frame bounds
are all checked.

All sixteen native/original comparisons pass. Maximum native/original
differences, in SPC cycles:

| Observation | Maximum absolute difference |
| --- | ---: |
| Preceding gate duration | 2842 |
| Onset interval | 2088 |
| Final stop interval | 2654 |
| Boundary pitch-to-stop interval | 34 |
| Final key-register write offset | 139 |

The retained allowance remains 4096 cycles for every native/original timing
comparison. No timer is restarted, score pulse discarded or timestamp shifted.
The original held-note evidence comes from accepted register writes, not copied
private PCM or independently sampled original envelope trajectories.

All 34 selected CTest suites passed, including the eight new final-boundary
tests and the preceding score regressions. The new image also passed 136
comparisons against retained pending, reverse, peer and following-rest original
observations, for 152 native/original comparisons including the 16 new final
cases. The bundled prototype image remains unchanged.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_final_probe
ctest --test-dir build-dmg-firmware -R 'native_score_final$' --output-on-failure
python3 scripts/check_sgb_score_final_playback.py \
  --probe build-dmg-firmware/gameboy_sgb_score_final_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-final-reference.json
```

Original-only measurements use `check_sgb_score_final_reference.py`. For
unchanged sanitized observations, the playback checker accepts
`--reference /tmp/score-final-observations.json` instead of `--trace` and
`--firmware-dir`, checking the owned cartridge SHA-256 before comparison.
Every original child retains its 8-million-instruction, 180-second, 16 MiB CSV
and fewer-than-32768-row bounds. The new post-stop metadata permits up to six
million observed SPC cycles to represent SGB2's longer remaining trace window;
this changes no execution cap or timing tolerance. Private instructions,
scores, instrument tables, samples and PCM are not inspected or copied.
Schemas are `gbb-score-final-observation-v1`, `gbb-spc-score-final-v1` and
`gbb-score-final-playback-reference-v1`.

This remains an opt-in direct-RAM/IPL-trampoline diagnostic with qualification
and playback false, outside bundled and production selection. SGB1/SGB2 program
ROMs remain required. Hardware checks, original PCM equivalence, broader driver
commands and real-title replacement qualification remain open.

Next, measure final termination with an already sounding peer, especially a
voice held across an inactive pattern. Determine which voices the FF/00 stop
pulse actually releases and whether a pending note retriggers before extending
the current preceding-release and single-pattern guards.
