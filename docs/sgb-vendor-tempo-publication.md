# Earlier APU publication and startup overlap

The [handoff correlation](sgb-vendor-tempo-handoff.md) found no music-value write
before the authored E7 operand read. This experiment advances request arrival
by another frame and adds finer offsets, preserving interrupted playback and
missing reads as explicit outcomes. It changes no firmware timing or image.

## Method

The first song starts at tempo 128 and has an authored E7 operand at `$2B70`
after three short notes. Matched fixtures set that operand to 32 or unchanged
128. The second song has a distinct pitch. All score/sample bytes are owned.
The 56 coordinates cross both live tempos with these schedules:

| Model | Request gaps (frames) | Arrival spin counts |
| --- | --- | --- |
| SGB1 | 7, 8 | 0, 256, 512, 768, 1024, 1536, 2048 |
| SGB2 | 3, 4 | 0, 256, 512, 768, 1024, 1536, 2048 |

The fixture builder now admits boundary gaps 3 through 12 and integer spin
counts 0 through 2499. Earlier valid fixture images remain unchanged. Arrival
spins affect which frame delivers the request; observed publication remains
frame-quantized rather than advancing continuously with each spin count.

The separate opt-in probe mode `--tempo-publication` observes `$2B70` after the
first old-song KON even when delivery counter 2 already applies. Existing
`--tempo-boundary` behavior stays unchanged. Actual delivery counters are
retained: old-song notes are never relabeled as new-song playback. Startup
validation checks the authored pitch prefix, binding, gates and selection
release, directory/traversal ownership, stable timer configuration and selected
song completion. It does not invent a request-1 playback/timer window when
startup crosses delivery 2. All music-write candidates and matching replies
remain visible, including ambiguous repeated writes.

```sh
python3 scripts/check_sgb_vendor_tempo_publication_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

Each immutable case runs for 300 million SNES clocks, with a 240-second child
timeout and 96-KiB output bound; four run concurrently. Schema
`gbb-sgb-vendor-tempo-publication-v1` records `qualification: false` and
`parameters_fitted: false`. Original execution stays opaque. Only public DSP,
fixed authored-address reads, equality flags and bounded timer metadata are
observed. Child failures suppress diagnostics and partial reports. Private
originals and local reports are not committed.

## Local original results

All 56 cases passed, forming 28 changed/unchanged pairs.

| Outcome | Cases | Changed-tempo wait | Unchanged-tempo wait |
| --- | ---: | --- | --- |
| Startup overlap, no post-start operand read | 18 | 20.003–20.454 ms | 20.003–20.454 ms |
| Every music write after the operand read | 38 | 31.998–32.407 ms | 20.001–20.009 ms |

Startup overlap occurred at SGB1 gap 7 with spins 0/256/512, and SGB2 gap 3
with spins 0/256/512/768/1024/1536, for both live tempos. Each emitted two old
notes under delivery counter 2, then the selected song. All nine paired wait
differences were exactly zero. This is evidence of an interrupted prefix with
no observed post-start E7 read, rather than an established-playback publication
crossing. In the other 19 pairs, changed/unchanged differences were
11.989–12.403 ms, consistent with the existing frozen 32/20-ms predictions.

Handoff categories were 26 `operand_before_write`, 18 `operand_not_observed`
and 12 `ambiguous_music_writes`. Every candidate in the 38 observed-read cases
followed the read. No case demonstrated publication before an observed operand
read. No timer configuration writes occurred during these observations.

The 56 owned controls passed, as did four observation transparency pairs and
four native/scalar reset/save-load playback pairs. Four additional private
original observer comparisons preserved earlier metadata exactly. Both generated
firmware images remain byte-identical. These results do not qualify a new
selection timing model or justify a production timing change.

The [delayed-tempo follow-up](sgb-vendor-delayed-tempo.md) now lengthens the
authored pre-change notes while preserving the operand address. It retains
missing reads and repeated publications separately, testing the first-write
hypothesis without relabeling an ambiguous handoff as uniquely accepted.
