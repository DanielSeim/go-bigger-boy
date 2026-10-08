# Opaque duration-4 direct-return observations

Both original models retrigger the held channel-2 voice when it returns with
a duration-4 note, then release that returning note before final readiness.
The gate is nearly identical at articulations 63 and 127. This adds the short
note evidence needed after the
[duration-8/16 native direct-return diagnostic](sgb-native-score-direct-return.md).
These measurements preceded the
[guarded native short-return diagnostic](sgb-native-score-short-return.md),
which now implements and checks this bounded profile.

## Independently authored corpus

`build_sgb_score_short_return_fixture.py` derives the preceding owned direct
return banks and changes only the two leading third-pattern durations to 4.
The banks remain 2048 bytes. Their first two patterns preserve articulation
127, tempo 96, instrument 2, pan 10, track volume 127 and song volume 160.
Channel 2's duration-24 note at tick 16 is clipped at tick 32, then remains
keyed while channel 3 alone plays notes 9A/9B through tick 64.

At tick 64, channel 2 executes A0, pitch 1700, while channel 3 executes C9.
Both have duration 4 and articulation 63 or 127. At tick 68, channel 2 executes
ED 64 and a pending A1 note, pitch 1800, or C9 rest. The pending duration remains
8 or 16, with the same returning articulation. Channel 3 executes ED 64 and
terminates the score on that tick. This isolates the new duration-4 note from
an unknown duration-4 pending-final profile.

The independent symbolic scheduler confirms pattern starts 0/32/64, channel
masks 12/8/12, nine expanded note/rest records and final score tick 68. The
pending event retains its own duration 8/16 as a terminal boundary record.
Two articulations, two pending durations, two final event kinds and both
original models give 16 fresh bounded reference executions. Every observation
is bound to its owned cartridge hash.

## Measured release and retrigger

All cases issue returning KON 4 without an intervening channel-2 KOF. The ledger
retains one unreleased retrigger from onset index 1 to onset index 4. Its raw
interval is 262274..262276 SPC cycles and equals the sum of the three intervening
onset intervals. No old-note release is invented at clipping, inactivity or
return. The returning note's subsequent gate is:

| Returning articulation | Returning duration | Timer KOF gate in SPC cycles |
| --- | --- | --- |
| 63 | 4 | 7426..7427 |
| 127 | 4 | 7432..7433 |

Changing the pending duration between 8 and 16 leaves those measured returning
gates unchanged. The roughly six-cycle articulation difference is small
relative to a 2048-cycle timer pulse. These are directly measured gate
intervals, not a newly qualified pulse-count table or interpolation law.
The shared reference checker admits the 6144..10240-cycle short-gate window
only with an explicit short-return selector. Its preceding defaults still
reject duration 4. The general native note table remains unchanged.

All 96 observed releases in the matrix are timer KOFs: five preceding releases
and the new returning-note release per execution. Every returning note releases
before final termination, with no voice pending at the final FF write. The
native gate count and owned envelope behavior were subsequently checked in
the separate native diagnostic; these original-only measurements do not
establish them.

## Actual setup and final readiness

Return setup is unchanged from the longer direct-return corpus. Both originals
write pitch before actual volume, with the same sequence and offsets throughout
the matrix:

| Offset from returning KON in SPC cycles | DSP address | Value |
| --- | --- | --- |
| -1479 | PITCHL2, 34 | 164 |
| -1443 | PITCHH2, 35 | 6 |
| -651 | VOLL2, 32 | 7 |
| -509 | VOLR2, 33 | 7 |

Returning controls remain KOF zero at offsets -156 and -39, followed by KON 4
at offset zero. Neither the channel-3 rest nor the deferred ED 64 writes DSP
volume or pitch. The pending A1 writes pitch 1800 before final completion,
retaining actual channel-2 volume 7/7. Channel 3 retains actual volume 1/1.
Final controls remain KOF FF, KOF zero 117 SPC cycles later and KON 4 for the
pending note or zero for the pending rest at offset 156.

Final stop occurs 18769..18777 SPC cycles after returning KON for pending notes,
or 18246..18254 for pending rests. The short raw KON-to-stop contract spans
8..12 whole timer pulses to cover setup/termination phase; it does not shift
any timestamp or introduce a fitted delay. The final note onset is separately
bound to the observed stop interval and final KON offset.

All eight pending notes remain unreleased during the bounded post-stop window;
all eight pending rests leave no unresolved voice. Those windows span
2759723..4511800 SPC cycles. Maximum intermodel differences are 49 cycles for
note gates, 48 for onset intervals, six for final stop, two for the retrigger
interval and zero for setup, control and pending-pitch offsets. All are within
the unchanged 2048-cycle intermodel allowance. The returning short gate itself
differs by one cycle between models.

These observations establish register-write lifecycle and timing. They do not
establish original ENVX trajectories, attack-reset behavior, PCM equivalence,
indefinite sustain, hardware behavior or real-title firmware qualification.
Private original instructions, scores, instrument tables, samples and PCM are
not inspected or copied.

## Validation and reproduction

```sh
python3 tests/sgb_score_short_return_tests.py
python3 scripts/check_sgb_score_short_return_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-short-return-observations.json
```

Eight new public tests cover owned fixture/input bounds, independent pending
durations and symbolic final readiness, the synthetic two-model ledger,
rejection by the preceding contract, forged gate/source/control/setup/tail
metadata, raw retrigger/final-onset binding, model/hash completeness, intermodel
drift and trace header/byte/row/order/integer bounds. The focused preceding
fixture, scheduler and physical direct/mixed-return tests pass. The preceding
native image still passes its 16 retained direct-return reference comparisons.

The optional local-reference CTest executes the original-only short-return
matrix. Its schema is `gbb-score-short-return-observation-v1`, with qualification
and playback false. The short gate and pending duration have separate metadata
fields: `return_duration` is 4 and `boundary_duration` is 8/16. Original children
retain the 8-million-instruction, 180-second, 16 MiB CSV and fewer-than-32768-row
caps. Source and native bounds remain unchanged.

All eight short-return banks were silently rejected by the preceding native
image: status 226, no accepted key writes and no nonzero owned PCM. That
4089-byte program retains SHA-256
`11eba7bf28920d59fd0278de5149263fc6803328554d4c030f8cd2e2f998440a`;
the bundled prototype is unchanged. SGB1/SGB2 program ROMs remain required.

The subsequent native diagnostic preserves these independent pending-final
events and the unreleased-retrigger ledger. Its implementation and separate
owned-envelope, audio and lifecycle evidence are documented in the linked
native profile.
