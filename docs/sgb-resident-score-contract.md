# Original resident score contract

This document records the resident memory layout and broader **offline syntax
oracle**. The explicit original [GBS1/GBS2 subsets](sgb-uploaded-score-v5.md),
[GBS3 tracks](sgb-uploaded-score-tracks.md) and [GBS4 phrases](sgb-uploaded-score-phrases.md)
and [GBS5 repeats](sgb-uploaded-score-repeats.md) now render on SPC; the broader grammar remains offline. No proprietary title,
PCM, instrument-bank or firmware bytes are checked in, and passing the oracle
does not qualify playback.

## Resident memory layout

The independent resident implementation reserves the following inclusive
SPC ranges. These are our allocations, not a reconstruction of vendor firmware.

| Range | Purpose |
| --- | --- |
| `$0000..00FF` | Direct-page variables and SPC I/O |
| `$0100..01FF` | Stack |
| `$0200..02FF` | Cold entry and legacy restart trampoline |
| `$0300..03FF` | Reserved resident workspace |
| `$0400..04FF` | GBS1/GBS2/GBS3/GBS4/GBS5 validation entry and silent rejection handler |
| `$0500..07FF` | Original directory, samples, effect module and legacy motifs |
| `$0800..0FFF` | Reserved workspace; current track contexts use direct page `$50..68` |
| `$1000..2AFF` | Resident driver and decoder code |
| `$2B00..4AFF` | Uploaded score data |
| `$4B00..FFBF` | Unallocated; echo writes remain disabled |
| `$FFC0..FFFF` | IPL overlay |

The driver lives at `$1000`; `$0200` selects legacy v4 and jumps there. `$0400`
validates explicit GBS1/GBS2/GBS3/GBS4/GBS5 banks before selecting v5/v6/v7/v8/v9. Its rejection handler
clears output readiness/version/signature, reports E1/E2 on output port 2, stops
timer 0 and mutes/resets the DSP. It loops without advertising compatibility.
The host observes the code in WRAM `$28` and remains external; a later SOUND or SOU_TRN
halts explicitly under the existing external ownership contract. This guard does not claim vendor score compatibility.

The generated payload contains 9525 contiguous bytes starting at `$0200`.
Relocation tests extract the actual driver rather than copying the entry stubs,
preserving the 766-byte token-alias case. The larger upload delays GB release;
tests wait for completed SOUND handshakes before measuring individual voices,
and allow the startup/upload time before checking full command sequences.
Existing generic
SOU_TRN uploads may install arbitrary drivers: they must retain their external
ownership path. A score-only restart must validate every block destination before
writing, rejecting any write outside the score region on that future path. Other jumps
must not be silently redirected to the score engine. The old `$07D0` motif upload
and `$0200` restart remain a separate, tested legacy contract.

## Restart and ownership requirements

The eventual rendering `$0400` entry must stop voices and clear pending mailbox parameters,
timer accumulators, track cursors, held notes and remembered effects while keeping
uploaded bytes. It must reset musical controls to documented independent defaults
and validate the selected score before enabling voices. A malformed or unsupported
score must remain silent with a bounded diagnostic; it must not advertise success.

The implemented GBS1/GBS2/GBS3/GBS4/GBS5 renderer advertises mailbox v5/v6/v7/v8/v9 only after validation.
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
GBS4 defines an original two-track barrier and fresh phrase state. Broader
cross-channel termination, tempo changes, quantization and vendor instrument
lookup still need rendering contracts. The
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

Real transfer tests now cover the reserved restart on both models, silence,
absence of adoption, rejection of subsequent SOUND, scalar parity, reset and
save/load during upload and after handoff. The full legacy firmware, relocated
uploads and motif-rendering tests remain the regression gates.

Next, extend the rendering grammar and define instrument/song-table contracts
with independent fixtures and private reference evidence. The title-demand blocker remains open until the
required score grammar, original bank behavior and private reference playback
have been validated.
