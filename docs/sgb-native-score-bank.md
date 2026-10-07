# Experimental multi-page score banks

The isolated bank renderer removes the 128-byte, single-page source restriction
from [two-voice instrument-2 playback](sgb-native-score-envelope.md). It accepts
1..2048 bytes at `$2B00`, with bounded 16-bit phrase, table, stream, call-target
and return cursors. Its log and caches no longer overlap uploaded data.

This addresses a prerequisite exposed by the observed 1619-byte Donkey Kong
upload. It does not qualify that title or its score grammar. The driver remains
outside bundling and production selection; SGB1/SGB2 program ROMs are required.

## Memory ownership

| Region | Owner |
| --- | --- |
| `$0800..137F` | Independently written diagnostic code, tables, owned directory/sample and padding |
| `$2B00..32FF` | Read-only score source pool, up to 2048 bytes |
| `$4000` page | Bounded 160-byte event log |
| `$4100` page | Four 32-byte expanded duration/opcode track slots |
| `$4200` page | Gate pulses at each duration/opcode pair offset |
| `$4300` page | Inherited articulation at each pair offset |
| `$4400/$4500` pages | Per-event left/right volume |
| `$4600/$4700/$4800` pages | Per-event pan/track/song volume |

Only the used prefixes of cache pages are written. Source, code, directory and
sample regions are separate; echo remains disabled. The preceding images retain
their layouts and hashes. The new probe poisons unprovided source bytes and the
gap surrounding the relocated caches, then requires the complete physical source
pool and guards to remain unchanged through rendering, quiet tail, reset and
restore. Even a compact input supplied to the preceding cache layout fails
this physical ownership check.

The private diagnostic input supplies length low at direct-page `$20` and high
at `$8E`. Native state uses `$21/$90` for the absolute cursor, `$91/$92` for
phrase/table high bytes, `$93/$94` for call-target/return high bytes, `$95/$96`
for the exclusive bank end, `$97/$98` for indirect-read scratch, and `$99/$9A`
for a global byte-read budget. These are not a production upload/mailbox API.

Every pointer must fall in `$2B00..end-1`. Every byte read checks the exclusive
end, including the second byte of a word or an opcode operand. Cursor increments
carry into the high byte. Phrase/table restoration and repeated calls/returns
restore both bytes. The native expansion also limits successful byte reads to
8192 across the complete bank validation; its other grammar bounds remain.

## Retained grammar and rendering

This change broadens source addressing and memory ownership. It retains exactly
two patterns, channels 2/3, 1..8 expanded events per track, one nonnested call
per track with counts 1..3, at most 32 executed controls per expanded track,
and 32 events/2032 ticks overall. Instrument 2, octave notes 24..36, measured
pan/volume points, both articulations, all ten gate profiles, rests, first-end
clipping, prevalidation, ADSR and quiet rejection keep their existing contracts.
Unsupported pointers, lengths, opcodes, profiles and truncated operands must
reject before any live event, KON or PCM.

`firmware/sgb/score_bank.asm` implements the bounded source read, pointer and
length routines. `build_sgb_score_bank.py` composes the owned preceding source,
adds full-pointer save/restore hooks, and relocates every cache read and indexed
store. The strict assembler checks overlap and branch/operand bounds. The
reproducible 2944-byte image at `$0800` has SHA-256
`1477d2c091c26d8fa49af3ec8113f411cdabe797a8311dd609278438914d3ffb`.
The authored BRR/directory bytes and preceding envelope image are unchanged.

```sh
python3 scripts/build_sgb_score_bank.py --output /tmp/score-bank.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_bank_probe
ctest --test-dir build-dmg-firmware -R 'native_score_bank$' --output-on-failure
```

## Owned page-crossing fixtures

`build_sgb_bank_fixture.py` relocates the same newly authored `pan-calls` and
`song-calls` sequences into 1619-byte and 2048-byte layouts. Unreferenced padding
is owned `$A5` data. The sequences retain eight paired onsets, body controls,
two executions of the shared body, inherited return settings, and explicit
second-pattern controls.

| Object | Offset within bank |
| --- | --- |
| Root word | `$0000` |
| Phrase | `$00FF`, first word crosses a page |
| Pattern tables | `$01FB/$03FB`, channel-2 words cross pages |
| First-pattern channel 2/3 | `$02F1/$04F5`, EF target words cross pages |
| Shared callee | `$05FF`, first control operand crosses a page |
| Second-pattern channel 2 | `$06FD` for 2048 bytes; `$05ED` for 1619 |
| Second-pattern channel 3 | Last nine bank bytes; terminator is the last byte |

This places live source data at and across the former log/cache addresses,
including `$3000/$3100/$3200`. Large raw size alone does not count as evidence;
actual reads, calls, repeats, returns and events must follow the relocated
objects correctly. An independent symbolic scheduler checks the resulting
note, articulation and control timeline.

The seventeen ROM-free tests include the complete preceding mix/envelope suite,
both bank sizes/cases/articulations, and page-crossing execution under all ten
gate profiles. They reject same-low-byte/wrong-high-byte pointers, pointers
before the bank or at/after its end, cache addresses used as source pointers,
partial words/controls at the exclusive end, targeted truncations around every
used page boundary, and 2049-byte input. Every compact fixture prefix remains
covered by the inherited truncation checks. Whole-engine/cross-engine restore
and reset compare actual events, KON/KOF, ADSR summaries and owned PCM hashes;
source/guard preservation is checked in the real APU component.

## Fresh private evidence

```sh
python3 scripts/check_sgb_score_bank_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_bank_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Eight fresh opaque original runs use the full 2048-byte fixtures, both original
models and both articulations, delivered by real JOYP/SOU_TRN transport. The
observer pins setup, pitch and volume at every KON and requires all 128 per-voice
KOF releases to be directly observed. Native/original onset spacing differs by
at most 1962 SPC cycles, gate lengths by 2446, and original model onset spacing
by 9. The existing 4096-cycle native/original and 2048-cycle inter-model allowances
remain unchanged, as do the original 84000..92000-cycle onset bounds.

| Case | Articulation | Owned 2048-byte fixture cartridge SHA-256 |
| --- | --- | --- |
| pan-calls | 127 | `f96dd4bd48b5386649098b85b7e645307fd454e62c1c27a14500995b6c303dbd` |
| pan-calls | 63 | `b1c6c53800f22c24c9a1f6001f6d1ff09088cb9605a54489065b87370f532f05` |
| song-calls | 127 | `322258b0afb181418e3baa3373b011aead0a597ec6eef025acf2a6fd697cfd78` |
| song-calls | 63 | `83179be5588c042cba8b45a30521037ed508b853e0f37695a18e459b9e604101` |

Private original firmware is executed without inspecting instructions, samples,
private scores or instrument tables. The existing 8000000-instruction and
180-second child limits, 16-MiB temporary trace bound and fewer than 32768 rows
remain in force. Only sanitized register/timing metadata and hashes are exported.
Original internal ENVX curves, PCM equivalence, physical-device and independent
emulator comparisons remain outside this evidence.

The new bank suite and nineteen preceding score/fixture regression suites
passed. The bundled prototype passes its reproducibility check and is unchanged.
Schemas are `gbb-spc-score-bank-v1` and `gbb-score-bank-reference-v1`, with
qualification and playback false. Installation remains an owned IPL trampoline
and direct-RAM diagnostic. The private original transport check does not qualify
replacement whole-system upload or production ownership. Broader patterns,
channels, instruments and real-title playback remain ahead.
