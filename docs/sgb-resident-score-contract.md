# Original resident score contract

This is the design and executable **offline syntax oracle** for the next original
SPC score interpreter. The current GBB v4 firmware does not implement this layout,
restart entry or grammar. No uploaded title, PCM, proprietary instrument bank or
firmware code is checked in, and passing the oracle does not qualify playback.

## Proposed memory layout

The independent resident implementation will reserve the following inclusive
SPC ranges. These are our allocations, not a reconstruction of vendor firmware.

| Range | Proposed purpose |
| --- | --- |
| `$0000..00FF` | Direct-page variables and SPC I/O |
| `$0100..01FF` | Stack |
| `$0200..02FF` | Cold entry and legacy restart trampoline |
| `$0300..03FF` | Reserved resident workspace |
| `$0400..04FF` | Uploaded-score restart entry |
| `$0500..07FF` | Original directory, samples, effect module and legacy motifs |
| `$0800..0FFF` | Per-track cursors, durations, controls and scheduler state |
| `$1000..2AFF` | Resident driver and decoder code |
| `$2B00..4AFF` | Uploaded score data |
| `$4B00..FFBF` | Unallocated; echo writes remain disabled |
| `$FFC0..FFFF` | IPL overlay |

Migration must move the existing driver out of `$0400`, regenerate the payload,
and update relocation tests before adding that restart entry. Existing generic
SOU_TRN uploads may install arbitrary drivers: they must retain their external
ownership path. A score-only restart must validate every block destination before
writing, rejecting any write outside the score region on that path. Other jumps
must not be silently redirected to the score engine. The old `$07D0` motif upload
and `$0200` restart remain a separate, tested legacy contract.

## Restart and ownership requirements

The eventual `$0400` entry must stop voices and clear pending mailbox parameters,
timer accumulators, track cursors, held notes and remembered effects while keeping
uploaded bytes. It must reset musical controls to documented independent defaults
and validate the selected score before enabling voices. A malformed or unsupported
score must remain silent with a bounded diagnostic; it must not advertise success.

Only an implemented SPC renderer may advertise the next GBB mailbox version.
The host must require a fresh signature and zero-token arm before it treats SOUND
as resident-owned again. Offline acceptance or arrival at `$0400` cannot establish
ownership. Song-ID table placement, built-in score selection and instrument-ID
mapping still need separate contracts; this milestone does not guess them.

Reset must recreate the original bank and initial state. Save/load must preserve
uploaded bytes, all track/scheduler state, staged commands and DSP history. Tests
must compare continuation samples, including a state saved during restart,
on SGB1 and SGB2 before a mailbox-version change is accepted. Native and combined
audio, scalar execution and repeated uploads remain required integration gates.

## Executable data subset

The independently written [decoder](../scripts/decode_sgb_score.py) follows public
data descriptions from [SnesLab](https://sneslab.net/wiki/N-SPC_Engine) and the
[Super Famicom Development Wiki](https://wiki.superfamicom.org/nintendo-music-format-(n-spc)).
It accepts a raw bank at `$2B00` and an explicit phrase-list address. No song-table
location or SGB-specific variant is inferred.

Phrase words are little-endian: zero ends the song; other supported words point
to eight track pointers. A zero track pointer is inactive. Track zero ends that
track. Duration bytes `01..7F` may have an articulation byte below `80`; notes
`80..C7`, tie `C8` and rest `C9` consume the current duration. Duration and raw
articulation persist per channel across patterns. Ties require a held note.
Supported one-byte controls are instrument `E0`, pan `E1`, song volume `E5`, tempo
`E7`, signed channel transpose `EA` and channel volume `ED`.

Everything else fails explicitly, including phrase loops, track subroutines,
percussion, fades, modulation and echo. Reads must fit the supplied bank, which is
at most 8192 bytes. Expansion stops at 64 patterns, 1024 events or 4096 read
operations. No pointer wraps or fallback reads from implicit resident RAM occur.
CLI errors return 2 with no partial JSON.

Output reports track-local ticks and raw controls, not scheduled DSP events.
Cross-channel phrase termination, tempo-to-time conversion, quantization, pitch,
instrument lookup and control application belong to the future renderer. The
JSON always reports `qualified: false` and `playback: false`.

## Original fixtures and checks

[score_subset_example.json](../firmware/sgb/score_subset_example.json) contains
original address/hex segments for two patterns, two channels, note/tie/rest,
duration and control changes. Its second pattern checks inherited track state.
It is an offline fixture, not a runnable SOU_TRN package for GBB v4.

```sh
python3 tests/sgb_score_decoder_tests.py
python3 scripts/decode_sgb_score.py /tmp/original-score-bank.bin --phrase 0x2B10
ctest --test-dir build-dmg-firmware -R gameboy_sgb_score_decoder_contract --output-on-failure
```

The tests construct raw bank bytes from the original segments, assert decoded
events and positions, and reject unsupported commands, missing durations,
invalid ties, out-of-bank pointers and every truncation of a complete track.
They also exercise all work limits and the CLI success/error contract. These are
ROM-free syntax checks; they neither execute SPC code nor compare reference audio.

Next, implement the relocated resident entry and the first real SPC rendering
subset against these fixtures. The title-demand blocker remains open until the
required score grammar, original bank behavior and private reference playback
have been validated.
