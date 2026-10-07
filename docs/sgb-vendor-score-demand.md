# Vendor score demand before playback

`scripts/inventory_sgb_score.py` scans a caller-supplied raw bank based at `$2B00`
and an **explicit** phrase-list address. It emits structural metadata, not score
events, note sequences, duration values, acoustic parameters, samples or PCM:

```sh
python3 scripts/inventory_sgb_score.py /tmp/private-score-bank.bin --phrase 0x2B06
```

The limited common N-SPC width profile uses public format research from
[SnesLab](https://sneslab.net/wiki/N-SPC_Engine). The public
[Pan Docs transfer contract](https://gbdev.io/pandocs/SGB_Command_Sound.html)
identifies the score region and `$0400` restart; it does not establish a particular
song-table root, instrument mapping or variant command grammar. The scanner
neither discovers roots nor asserts that this common profile matches SGB firmware.
The existing strict decoder and GBS1..GBS6 renderer retain their narrower grammar.

## Structural scan contract

Phrase words point to eight-channel pattern tables. Zero ends the list. A phrase
control stops that list with an explicit frontier. Each nonzero track pointer is
scanned independently to zero or an unsupported boundary. The scan recognizes
duration/optional articulation, notes, ties, rests, percussion and the operand
widths of E0, E1, E3, E5, E7, E8, EA, ED, EF, F4, F5 and F7. It skips operands
without rendering their acoustic effect. Unknown commands stop that track;
other tracks remain available for inventory. E6 is deliberately unsupported
because generic public descriptions disagree on its operand width.

EF reads only its three operand bytes, records the subroutine target, then stops
that path. Neither the subroutine body nor the caller's continuation is scanned.
An out-of-bank target can be reported as metadata; it is never followed or
validated as an executable call. Phrase controls are likewise never expanded.
These boundaries make the observed demand a **lower bound**, not a complete song.

Output includes a bank hash, pattern/track reference counts, observed channel and
instrument IDs, aggregate base-note range, structural opcode counts, boundaries
and required rendering contracts. Instrument selectors and subroutine target
addresses are retained as structural metadata. Repeated pattern references count again;
these are structural counts, not scheduled events or playback duration.
`linear_scan_complete` means only that the supplied paths ended without a
boundary. It does not validate held-note state, resolve cross-channel phrase
termination, prove song selection, or qualify driver behavior.
`qualification` and `playback` always remain false.

Inputs are limited to 8192 bank bytes; CLI reads stop at 8193. Each scan allows
64 patterns, 8192 scanned opcodes and 32768 reads. Truncated data, out-of-bank
reads, invalid duration encoding or budget exhaustion fail with status 2 and
no partial JSON. Unknown commands and subroutine/phrase boundaries produce a
partial report with status 0, distinguishing unavailable semantics from read errors.

## Private Donkey Kong evidence, 2026-10-06

The current prototype (`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`)
was run cold, without input or initial battery RAM, against the same local
Donkey Kong (JU) v1.1 input pinned in the [title audit](sgb-original-title-demand.md).
The clock bound was 90000000 for each model. Both captured the same 1619-byte
bank at `$2B00`, followed by jump `$0400`; its SHA-256 was
`74102e34fdae96e07d0e9cd831e174fdaa9f780140a2c98008f972abf0db1701`.

Both halted on the subsequent SOU_TRN at frame 140: SGB1 at 72719466 clocks,
SGB2 at 74351028. Each completed one capture and handoff, reported no transfer
error, retained only the cold v4 adoption and remained externally owned.
This refresh confirms the vendor-bank rejection is still open after GBS6.

The capture used temporary local diagnostic instrumentation, not a shipped
probe export interface. No private bytes or generated private reports are
committed. These checks require the private game input and are not CI gates.
No original-program PCM, independent emulator or physical-device comparison
was run for this milestone.

The first three words in the bank point to candidate phrase roots `$2B06`,
`$2B31` and `$2B5C`. Their initial identification as a song table was an
**inference** from the common format and bounded structural walks. The
subsequent black-box evidence below validates a limited selection contract.

The [address-only selection observer](sgb-song-selection-observation.md) supplies
a bounded native reference capture for that evidence. It distinguishes prior
base writes and nonzero SOUND delivery counts without exporting directory bytes;
dummy reads during uploads must be excluded from selection inferences. Fresh
private original title runs on both models support code 1 selecting the first
word at `$2B00`; those title captures do not validate codes 2 and 3.

The subsequent [controlled directory fixture](sgb-song-selection-fixtures.md)
also validates codes 2 and 3, including reordered and repeated requests, against
both private original program images. Other selection and rendering contracts
remain open. The [controlled note fixture](sgb-pitch-instrument-fixtures.md)
now pins three notes and instruments 2/10 on channel 2 against both originals;
it does not supply an instrument table, samples or a vendor renderer. The
[tempo/articulation fixture](sgb-tempo-articulation-fixtures.md) additionally
measures duration-16 onset and key-off timing for two tempos and articulations,
with bounded two-model reference comparisons. The
[volume/pan fixtures](sgb-volume-pan-fixtures.md) now also pin center/endpoints
and reduced song/track volume register values. The
[echo fixtures](sgb-echo-fixtures.md) pin settled routing, two delays, feedback
and filter-0 setup with explicit rests; full curves, echo PCM and immediate
setup timing remain open. The [two-channel phrase fixtures](sgb-phrase-fixtures.md)
now support early phrase advancement when either of channels 2/3 ends, with
swapped durations and an equal-long control; broader channel behavior remains
unqualified.

| Explicit candidate root | Patterns / track references | Channels observed | Scan boundary |
| --- | --- | --- | --- |
| `$2B06` | 1 / 1 | 2 | Linear paths end |
| `$2B31` | 1 / 1 | 2 | Linear paths end |
| `$2B5C` | 10 / 66 | 0..7 | 50 subroutine boundaries |

Both short candidates contain song/direct volume, echo enable/setup, pan, tempo,
instrument selection and optional articulation. One also has a base pitch above
the GBS renderer's current 0..31 range. Even those short paths therefore need
vendor song selection, channel/pitch mapping, instrument/sample mapping, tempo,
quantization/velocity, volume/pan curves and echo contracts with behavioral gates.

The larger candidate additionally exposes fine tuning, vibrato, tempo fades,
all eight channel indices and subroutines. Its observed instrument IDs are not
the prototype's four direct SRCN choices. The scan intentionally cannot describe
the unvisited subroutine bodies or continuation demand.

The controlled gates now constrain selection and several short-entry renderer
contracts. The next implementation target is bounded vendor scheduling with the
measured phrase transition, an independently owned instrument/sample strategy
and lifecycle validation. The measured pitch/volume points do not qualify full
curves. Larger-score channel allocation and subroutine execution remain separate
work. Until these contracts and private
behavioral gates pass, the prototype must keep rejecting vendor uploads; it
must not acknowledge ownership or redirect `$0400` to diagnostic motifs.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(score_demand_inventory|score_decoder_contract|original_compatibility_report)' --output-on-failure
```
