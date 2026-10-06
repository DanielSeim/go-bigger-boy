# Original uploaded score playback

GBB mailbox v5 renders a bounded, independently authored GBS1 bank on the actual
SPC/DSP. This explicit envelope supplies the data entry point without guessing
a vendor song table. Nintendo N-SPC title data remains unsupported.

## Bank and restart

One upload writes at `$2B00` and jumps to `$0400`. The bank occupies at most
255 bytes and has this exact layout:

| Offset | Contents |
| --- | --- |
| `00..03` | ASCII `GBS1` |
| `04` | Total bank byte length, 35..255 |
| `05..07` | Zero |
| `08..09` | Little-endian phrase pointer `$2B10` |
| `0A..0F` | Zero: song terminator and reserved bytes |
| `10..11` | Channel-zero track pointer `$2B20` |
| `12..1F` | Zero: seven inactive channels |
| `20..end` | One finite track with zero as its final byte |

The independent validator at `$1400` checks the full header and stream before
selecting v5. Duration `01..7F` must precede the first event and may change later;
it must be followed by an event. Events are notes `80..9F`, tie `C8` and rest
`C9`. Ties require a held note and cannot follow a rest. The first zero must be
the bank's last byte. Articulation, controls, loops and other channels/pointers
are rejected. This is a deliberately smaller subset than the offline oracle.

Bad headers report E1, bad/unsupported syntax E2 on output port 2. Rejection
clears readiness, stops the timer, mutes/resets the DSP and remains external.
The host observes the code in WRAM `$28`; later SOUND/SOU_TRN halts explicitly.
General uploaded drivers retain their existing external ownership path and
destination checks. Validation checks the declared GBS1 data after handoff;
a separate score-only transfer path remains future work.

Only successful validation publishes `5A/C5/A5`; the host requires a fresh
zero-token arm. SOUND music `01` restarts this bank, `00` keeps it and `80` stops
it. Music `02` is invalid for v5. Effect/attribute limits remain those of v4.
IPL return clears v5 mode. Repeated valid banks may rearm; `$0200` selects v4
and its legacy motifs. Cold startup also remains v4.

## Rendering and authoring

The renderer at `$1600` uses DSP voice 4 and the original triangle BRR loop.
One tick is 16 ms at the default 1.024 MHz SPC clock. Notes cover C3 through G5;
pitch words at `$1700` derive from 440 Hz tuning and the 16-sample loop. Ties
retain the voice without KON, rests KOF it, and termination disables music and
clears its gain. Effects remain independent. No host-generated music PCM or
execution interception is involved.

State lives in SPC direct page: `$30` mode, `$31` end, `$32` cursor, `$33` duration,
`$34` countdown, `$35` held-note flag, `$36..38` validation/temporary values.
Existing whole-host states preserve these bytes, the uploaded bank and DSP
history; no serialization changes are needed. Reset recreates the initial bank.
The generated contiguous startup payload is now 5440 bytes, so tests allow its
upload before checking complete command sequences.

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_score_example.json --output /tmp/resident-score.bin
ctest --test-dir build-dmg-firmware -R gameboy_sgb_resident_score_contract --output-on-failure
```

The strict JSON packer accepts 1..111 events with integer `ticks` from 1 to 127
and one of integer `note` 0..31, `tie: true` or `rest: true`. It validates with
the independent offline grammar oracle, creates one score block plus a `$0400`
jump and zero-pads to 4096 bytes. It refuses existing output files. The checked-in
example is independently authored and intended for this original prototype.

## Evidence and remaining work

ROM-free integration checks perform real GB VRAM/ICD capture, IPL upload and
SPC execution on SGB1/SGB2. They measure an octave pitch ratio, held tie pitch,
silence during a rest, resumption and finite termination. They also cover bad
headers/lengths/channels/opcodes/ties, v5 limits, repeated banks and legacy restart,
native/combined scalar PCM/state equality, reset and active-note save/load.

The broader grammar oracle and resident layout remain documented in the
[resident score contract](sgb-resident-score-contract.md). Instrument mapping,
song tables, multiple tracks, controls and loops still need independent contracts
and reference evidence. No private title PCM parity is claimed by this milestone.
