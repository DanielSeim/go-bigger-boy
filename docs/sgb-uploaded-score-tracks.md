# Original two-track uploaded scores

GBS3 extends [GBS2 controls](sgb-uploaded-score-controls.md) with two independent
finite tracks rendered by the actual SPC/DSP. This opt-in diagnostic format uses
original fixtures and advertises mailbox v7 only after validating both streams.
The [GBS4 extension](sgb-uploaded-score-phrases.md) adds finite phrase sequencing.
These formats do not qualify vendor title playback or reproduce vendor phrase semantics.

The bank remains at `$2B00`, with at most 255 bytes. Its header is:

| Offset | Value |
| --- | --- |
| `00..03` | ASCII `GBS3` |
| `04` | Total bank length |
| `05` | Second track offset; also the exclusive first track end |
| `06..07` | Zero |
| `08..09` | Phrase pointer `$2B10` |
| `0A..0F` | Zero |
| `10..11` | First track pointer `$2B20` |
| `12..13` | Second track pointer `$2B00 + offset 05` |
| `14..1F` | Zero; six inactive tracks |

Both contiguous tracks use the existing duration `01..7F`, note `80..9F`,
tie `C8`, rest `C9`, and instrument/pan/gain controls `E0/E1/ED`. Each track
ends with exactly one terminal zero at its declared boundary. Validation resets
held-note and duration state between tracks; a second-track tie cannot borrow
from the first. Bounds, reserved bytes, pointers, complete operands and grammar
are checked before any v7 signature. Header errors report E1, stream errors E2,
and either rejection remains silent and external.

Track zero uses DSP voice 4; track one uses voice 3. Effect voices 6 and 5
remain separate. Each physical 16 ms timer tick advances both active tracks
once. Cursors, duration, countdown, held-note flag and instrument/pan/gain are
independent. A rest or end sets only that track's bit in the shared key-off mask;
notes and effect commands preserve the other track's bit. One track may end
while the other continues; the song ends when both finish. There are no phrase
transitions, loops, tempo changes or vendor instrument/song tables in this format.

SOUND music `00` keeps both tracks, `01` restarts both with source 1, center pan
and gain 80, and `80` stops both. Music `02` is rejected. Effect limits and the
fresh zero-token arm remain unchanged. Unsupported owned commands silence all
four voices. Cooperative IPL return and legacy `$0200` restart clear uploaded
mode; voice 3/4 gains are muted during driver initialization to prevent a prior
score from leaking into the legacy mode.

The dispatcher and context copies live at `$1C00`, dynamic DSP register selection
at `$1F00`, and the shared single-track player at `$2000` (with a `$1600`
trampoline). Contexts use direct-page `$50..58` and `$60..68`; `$43..45` select
the current voice, `$48` stores key-off bits, and `$4B/$4C` hold track ends.
The original contiguous startup payload is 9511 bytes from `$0200`; the generated
image remains reproducible and separate from production firmware selection.

The strict packer requires `"format": "GBS3"` and `"tracks": [events0, events1]`.
Each nonempty event array has the GBS2 schema. At most 110 events fit across
both tracks; the two terminators and 32-byte header must fit 255 bytes. The
independent offline decoder validates both tracks before the packer constructs
its 4096-byte SOU_TRN transport with `$0400` handoff. GBS1/GBS2 schemas and
no-overwrite behavior remain unchanged.

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_tracks_example.json --output /tmp/resident-tracks.bin
ctest --test-dir build-dmg-firmware -R gameboy_sgb_resident_score_contract --output-on-failure
```

The original example pans the two instruments apart and gives them different
note durations. Track zero rests, resumes at half gain and ends while track one
holds a tie, then changes pitch and finishes. Integration checks measure these
independent stereo transitions on both models in native and combined audio;
compare scalar PCM/state; restore across a rest, tie and first-track end; and
repeat reset, uploads, explicit stop and legacy restart. Malformed second tracks
and headers reject before adoption, and an invalid score request after concurrent
effect/music startup silences every voice. These checks use no proprietary assets.
