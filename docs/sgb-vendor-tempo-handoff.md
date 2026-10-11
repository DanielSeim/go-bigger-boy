# Tempo reads and APU handoff

The [boundary sweep](sgb-vendor-tempo-boundary.md) shows that a GB SOUND delivery
can precede the old track's authored E7 operand read while selection still uses
the new tempo. This experiment correlates that read with public APU music-value
writes and the first matching SPC reply, using the same 32 fixtures and schedules.
It changes neither firmware nor probe behavior.

The existing `--tempo-boundary` capture already includes bounded host writes,
host readback and SPC reply observations. The checker retains the boundary
sequence, gates, binding, ownership, traversal, timer and completion guards.
It additionally checks every handshake's exact fields, chronological request
ownership, port, latest-write interval, read/reply counts and timestamp bounds.
Authored SOUND controls are zero and music values are 1/2, so only equality
masks 0, 1 and 14 are admitted. Music-equal replies must agree with the generic
host-write-equal reply observations. No original port byte is exported.

Every request-2 write equal to its authored music field is retained, together
with its port, timestamp, matching reply count and first matching reply timestamp.
Signed deltas place the operand and selected-directory read relative to each
write/reply. A single candidate is classified as before, between or after the
write/reply anchors; equal timestamps are reported separately because the host
callback samples the current APU clock rather than exact fractional bus phase.
Repeated candidates remain `ambiguous_music_writes`. Missing writes/replies and
absent/ambiguous operands have explicit categories; the checker never chooses
a candidate to fit the timing result. A matching reply is an observable marker,
not proof of a specific command-acceptance instruction.

```sh
python3 scripts/check_sgb_vendor_tempo_handoff_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The report schema is `gbb-sgb-vendor-tempo-handoff-v1`, with `qualification: false`
and `parameters_fitted: false`. Each immutable fixture runs for 300 million SNES
clocks with a 240-second timeout and 96-KiB capture bound; four cases run
concurrently. Original execution stays opaque. Output contains only existing
public/owned-address metadata, equality flags and timestamps; failures suppress
child diagnostics and partial JSON. Firmware/fixture hashes are retained locally.

All 32 local original cases passed, and every earlier boundary-summary field
matched the previous report exactly. All 45 music-value write candidates were
later than the operand read. Nineteen cases had one candidate; thirteen had two
and remain ambiguous. In each repeated-write case the first candidate had no
matching reply in its latest-write interval. The later candidate did; it is still
reported alongside the first, rather than substituted for it.

| Model | Live tempo | One write / repeated writes | First write after operand (ms) |
| --- | ---: | ---: | ---: |
| SGB1 | 32 | 3 / 5 | 0.052734 to 66.599609 |
| SGB1 | 128 | 8 / 0 | 0.056641 to 66.599609 |
| SGB2 | 32 | 0 / 8 | 3.160156 to 53.021973 |
| SGB2 | 128 | 8 / 0 | 3.160645 to 53.021973 |

All 32 first matching replies followed the final selected-directory read by
0.058594 ms. Thus those replies cannot serve as a pre-directory acceptance
anchor. Crucially, the three changed-tempo cases whose GB requests preceded the
operand read also published every music-value candidate after that read. The
previous delivery-boundary result therefore does not distinguish tempo retention
at APU publication from later sampling. No publication-before-operand case was
captured, and no firmware timing change is justified by this corpus.

The existing 32 owned boundary captures now validate the handoff metadata too,
without extra executions. Two short contracts exercise before/between/after and
shared-timestamp ordering, repeated writes, missing replies, ambiguous operands,
latest-write interval violations, strict fields and sanitized failures. The
existing observer transparency and native/scalar lifecycle controls are retained.
Both generated firmware images remain byte-identical.

Next, advance request arrival far enough to publish the music value before the
operand read, then use finer arrival offsets to bracket the transition. Retain
interrupted tracks and missing reads as outcomes. That experiment can test the
publication boundary that this corpus does not cross, before choosing a new
selection timing model.
