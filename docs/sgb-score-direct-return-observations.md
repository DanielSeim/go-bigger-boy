# Opaque observations of a held voice returning directly with a note

Both original models issue KON for a returning note while channel 2's old
clipped voice remains keyed. There is no intervening channel-2 KOF. The
returning note subsequently receives its own timer KOF before final score
termination. This establishes the register-write behavior needed to extend
the [mixed-articulation rest diagnostic](sgb-native-score-final-mixed.md).
It does not yet implement or independently qualify native direct-note return.

## Owned matrix

`build_sgb_score_direct_return_fixture.py` authors eight 2048-byte banks.
The first two patterns retain the preceding owned topology: articulation 127,
tempo 96, instrument 2, pan 10, track volume 127 and song volume 160. At tick
16, channel 2 plays a duration-24 note; channel 3's duration-16 note terminates
the first pattern at tick 32, clipping channel 2. Channel 3 alone plays notes
9A/9B at ticks 32/48, with actual volume 1/1 after its earlier ED 64.

At tick 64, channel 2 returns directly with A0, pitch 1700, while channel 3
executes a matching-duration rest. Both use articulation 63 or 127 and duration
8 or 16. At tick 72/80, channel 2 executes ED 64 and a pending A1 note, pitch
1800, or a C9 rest, with the same articulation and duration. Channel 3 executes
ED 64 and terminates the score on that tick. The independent symbolic scheduler
retains the pending event as a final boundary record. Pattern starts are
0/32/64, channel masks are 12/8/12, and there are nine expanded note/rest records.

Two returning articulations, two durations, two final event kinds and SGB1/SGB2
give a 16-case reference matrix. All cases pass the bounded observation
contract and the existing 2048-SPC-cycle intermodel allowance. Cartridge hashes
bind every observation to its independently authored fixture. Original programs
and IPL images execute opaquely; only owned DSP/timing metadata is retained.

## Returning-note ledger and setup

The channel-2 KON at tick 64 occurs 262274..262276 SPC cycles after its old
onset at tick 16. Its ledger entry is an unreleased retrigger from onset index
1 to onset index 4. That interval equals the sum of the three intervening raw
onset intervals. No old-note KOF is manufactured at clipping, inactivity or
return. The observed returning control writes, relative to its KON, are:

| Offset in SPC cycles | DSP write |
| --- | --- |
| -156 | KOF 00 |
| -39 | KOF 00 |
| 0 | KON 04 |

The return writes pitch before volume. The sequence and offsets match exactly
across all 16 original cases:

| Offset from returning KON | Address | Value |
| --- | --- | --- |
| -1479 | PITCHL2, 34 | 164 |
| -1443 | PITCHH2, 35 | 6 |
| -651 | VOLL2, 32 | 7 |
| -509 | VOLR2, 33 | 7 |

The subsequent channel-2 KOF releases the returning note rather than supplying
a missing release for the old onset. Its measured gates are consistent with
the existing note pulse profiles:

| Returning articulation | Duration | Gate in SPC cycles | Existing note pulses |
| --- | --- | --- | --- |
| 63 | 8 | 17662..17672 | 10 |
| 63 | 16 | 46338..46342 | 23 |
| 127 | 8 | 29958..29961 | 15 |
| 127 | 16 | 73379..73428 | 36 |

All 96 observed releases in the matrix are timer KOFs: five preceding releases
and the returning-note release per execution. The old channel-2 onset has no
separate release before its retrigger. Repeated KOF writes after a release do
not generate additional ledger releases. Raw onset spacing and gate timestamps
are retained. The raw returning-KON-to-final-stop contract allows one timer
pulse of setup phase around the preceding whole-pulse window; no timestamps
are shifted or fitted delays introduced.

## Final readiness and limits

Every returning note releases before final termination. No voice remains
pending at the final FF write. The final controls are KOF FF, KOF zero 117 SPC
cycles later, then KON 4 for the pending note or KON zero for the pending rest
at offset 156. A pending note writes pitch 1800 before that pulse and retains
actual volume 7/7 despite its deferred ED 64. No later DSP volume change occurs.
Channel 3 retains actual volume 1/1.

All eight final notes remain unreleased during the bounded post-stop window;
all eight final rests leave no unresolved voice. Those windows span
2693731..4489271 SPC cycles. Maximum intermodel differences are 49 cycles for
note gates, 48 for onset and stop intervals, two for the unreleased-retrigger
interval, and zero for setup, final-control and pending-pitch offsets.

These are register-write observations. They do not establish original ENVX
trajectories, attack-reset behavior, PCM equivalence, indefinite sustain,
hardware behavior or real-title replacement qualification. Original private
instructions, scores, instrument tables, samples and PCM are not inspected or
copied. Reverse articulation transitions and other return geometries are outside
this corpus.

## Reproduction and next implementation

```sh
python3 tests/sgb_score_direct_return_tests.py
python3 scripts/check_sgb_score_direct_return_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-direct-return-observations.json
```

Eight public tests validate fixture bounds/reproducibility, symbolic clipping
and final readiness, the synthetic two-model matrix, a forbidden pre-return
KOF, forged retrigger/gate/control/setup/tail metadata, model/hash completeness,
intermodel drift and trace header/byte/row/order/integer bounds. The focused
fixture, scheduler and preceding mixed-articulation diagnostic suites pass.
The optional local-reference CTest executes the original-only 16-case matrix.
Its schema is `gbb-score-direct-return-observation-v1`, with qualification and
playback false. Original children retain the 8-million-instruction,
180-second, 16 MiB CSV and fewer-than-32768-row caps.

The current 4089-byte mixed-articulation native image silently rejects all
eight direct-return banks: status 226, no accepted key writes and no nonzero
owned PCM. Its source/hash and the bundled prototype are unchanged. SGB1/SGB2
program ROMs remain required.

Next, implement a guarded direct-return diagnostic using this measured corpus.
Preserve the unreleased-retrigger ledger and the pitch-before-volume setup,
use the existing returning-note gate profiles, and separately validate owned
DSP envelope behavior at the retrigger. Check final held/silent audio,
reset/save/load, and all previously qualified profiles before any bundling.
