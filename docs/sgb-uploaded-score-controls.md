# Original uploaded-score controls

GBS2 extends the original [uploaded-score format](sgb-uploaded-score-v5.md) with
instrument, pan and volume controls on the actual SPC/DSP. The bank keeps the
same bounded single-channel layout and uses ASCII `GBS2`. Complete header and
operand validation selects mailbox v6; GBS1 still selects v5 and rejects controls.
Vendor title data remains unsupported.

| Opcode | Control | Operand |
| --- | --- | --- |
| `E0` | Original instrument / DSP SRCN | 0 square, 1 triangle, 2 pulse, 3 saw |
| `E1` | Original linear pan | 0..20: 0 left, 10 center, 20 right |
| `ED` | DSP direct GAIN | 0..127, including zero mute |

Controls consume one operand and no score time. They may precede the first
duration or occur between timed notes/ties/rests. Validation checks format,
operand availability and range before readiness. Invalid controls report E2 and
remain silent/external. Articulation, additional controls, loops and other tracks
remain unsupported.

These are independently defined rendering semantics. The implementation does
not claim vendor instrument mappings, pan curves or N-SPC volume arithmetic.
Pan uses a linear 96/0 to 0/96 law with 48/48 center. Gain is independent of pan.
Pan/gain changes write the held voice's registers without KON; instrument changes
write SRCN, and subsequent notes use the ordinary key-on path. Every music restart restores source 1, center pan and gain
80 before interpreting the stream. GBS1 receives the same original defaults.

Mailbox v6 has the same SOUND limits as v5: music 00 keep, 01 restart, 80 stop;
02 is invalid. Effects retain v4 limits. A fresh zero-token arm is required,
cooperative IPL return clears uploaded-score mode, and legacy `$0200` selects v4.

The control helper starts at `$1800`, operand validation at `$1900` and pan
tables at `$1A00`. SPC direct page `$39` holds an operand, `$3F` the format,
and `$40..42` the current instrument/pan/gain. These bytes, the cursor/countdown,
bank and DSP history are covered by existing whole-host save states. The
contiguous startup payload is now 6186 bytes; audio measurement waits for SOUND
acknowledgment and full sequences allow time for the upload.

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_controls_example.json --output /tmp/resident-controls.bin
ctest --test-dir build-dmg-firmware -R gameboy_sgb_resident_score_contract --output-on-failure
```

The strict packer accepts optional `"format": "GBS2"` and single-field events
such as `{"instrument": 0}`, `{"pan": 20}` and `{"volume": 32}`. Timed events
retain their existing schema. GBS1 is the default and rejects control events.
The same 111-event/255-byte limits, offline-oracle validation, 4096-byte transport
and no-overwrite rules apply. The checked-in controls example is original data.

Two-model native/combined integration checks measure half-amplitude gain,
balanced center and zero opposite-channel output at pan endpoints. They compare
all four BRR instruments at the same note/pan/gain, restore across a pending pan
change, and check reset, scalar PCM/state parity and repeated v6 adoption with
legacy restart. Malformed bounds, truncated controls, unsupported opcodes and
GBS1 control rejection fail before adoption. These are ROM-free checks, not
private title PCM comparisons. Multiple-track scheduling remains the next step.
