# Original bounded phrase repeats

GBS5 extends [GBS4 finite phrases](sgb-uploaded-score-phrases.md) with a bounded
whole-sequence repeat count. Its header uses ASCII `GBS5`, and byte `06` contains
**1..4 total plays, including the initial pass**. Byte `07` remains zero.
All other pointers, count limits, canonical tables, contiguous tracks and
instrument/pan/gain grammar remain unchanged. The bank still fits 255 bytes.
GBS4 continues to require zero at byte `06` and performs exactly one pass.

The repeat-count validator at `$2500` rejects zero, five or greater, and reserved
header data before any readiness. All patterns validate before `5A/C9/A5` is
published and the host performs its fresh zero-token arm. There is no partial
adoption of a valid first pass with malformed later data.

At every phrase boundary, both tracks must finish before the next starts. At the
last phrase's end, the remaining-play counter decreases. A nonzero counter
restarts at the first pattern during that same tick, using the usual fresh track
cursors, duration/countdown, held flags, source 1, center pan and gain 80. Authored
controls then apply normally. Zero remaining plays ends silently. There are at
most sixteen phrase starts per SOUND restart, even if every track contains only
controls and consumes no score time. There are no nested loops, arbitrary loop
pointers, unbounded repeats or vendor loop-format claims.

Direct-page `$7D` holds the validated total count and `$7E` the remaining count.
The existing sequence-reset helper at `$2680` replenishes `$7E`; the barrier at
`$2700` wraps without replenishing it. Whole-host save states preserve these
bytes alongside all track, bank, timer, mailbox and DSP state. The reproducible
contiguous startup payload is 9525 bytes from `$0200`. This remains an opt-in
diagnostic image; production firmware selection and external overrides retain
their existing behavior.

SOUND music `00` keeps playback, `01` restarts at the first pattern and replenishes
the total count, and `80` stops both tracks and prevents further wraps. Music
`02` remains invalid. Unsupported owned commands silence all music/effect voices
with their normal release tail. Cooperative IPL return, repeated uploads and
legacy `$0200` restart retain their contracts.

The strict authoring schema is `"format": "GBS5"`, `"plays": 1..4`, and
`"patterns": [[events0, events1], ...]`. The play count is mandatory and must be
an integer; booleans, fractions, strings and missing counts reject. GBS4 rejects
the new field. The independent oracle still validates every pattern with fresh
track state and checks the complete phrase list before packaging its 4096-byte
SOU_TRN transport. The CLI keeps its no-overwrite/no-partial-output rules.

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_repeats_example.json --output /tmp/resident-repeats.bin
ctest --test-dir build-dmg-firmware -R gameboy_sgb_resident_score_contract --output-on-failure
```

The original two-pattern fixture plays a low note, waits for a longer rest,
changes to an octave note at half gain, and repeats the sequence once. Actual
SPC/DSP integration checks on SGB1/SGB2 in native and combined audio measure both
passes and finite silence, compare scalar PCM/state, repeat reset playback, and
restore across the whole-sequence wrap. A restart during the second pass must
replenish the counter; an early stop must suppress all future wraps. Additional
fixtures check the fourth audible pass and sixteen immediate control-only phrase
starts. Invalid counts, a GBS4 count-byte violation and malformed later streams
reject before adoption; repeated uploads and legacy restart remain covered.
Vendor title scores, song/instrument tables and proprietary acoustic equivalence
remain unsupported and unqualified. No proprietary inputs are used by these checks.
