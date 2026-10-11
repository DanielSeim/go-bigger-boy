# Tempo changes while selection is pending

The [operand timestamp experiment](sgb-vendor-tempo-age.md) establishes a direct
bus anchor for the authored E7 at `$2B70`. This sweep places the next sound
request around that read while retaining original delivery-counter ownership
on every DSP event.

The independently authored fixture adds `boundary=True` / `--boundary`, changing
only the second song's note to `$98`, with previously measured uploaded-binding
pitch 717. The old track's A4/A3/A4/A3/A2 prefix uses pitches 1437/1357/1437/1357/
1281. This distinct selected pitch prevents a still-running old-track note from
being mistaken for the selected song's first KON. Starting tempo is fixed at
128; the late E7 sets 32 or repeats 128. Duration, instrument, assets, score
pointers and transport remain the existing authored controls. The new active
request-gap range is 4–12, with spins 0/1024; defaults preserve all older images.

`--tempo-boundary` is a separate opt-in probe mode. It extends the operand/timer
observers, records the fixed authored operand after the first song's KON even
when the delivery counter advances, and starts request 2's setup/continuity
window only on the distinct pitch 717. Events keep their actual request counter;
they are not relabeled as belonging to the old song. Ordinary observation modes
retain their previous first-KON behavior and bounds. Counts remain bounded at
64 operand reads, 128 DSP events and the existing traversal/handshake limits.
Overflow rejects the capture before JSON output.

The checker validates a prefix of the authored old-note sequence, its binding,
gates and starting cadence, the distinct selected note, complete live traversal,
timer windows/configuration and selected-song completion. The first song must
have started and must still be active when the second request arrives. The
collapsed setup/continuity validation retains only the old song's first KON and
the distinct selected KON; the full public DSP sequence remains intact.

Exactly one operand read must lie between the third old-track note and the
fourth note (or selection if interrupted). Its signed age determines
`operand_before_request` or `request_before_operand`, with the recorded delivery
counter resolving equal timestamps. No read is permitted only
when the old track was interrupted before its fourth note, reported as
`not_observed`. Multiple reads are `ambiguous_reads`, since an owned validator
can revisit score addresses; they are not reduced to a chosen read to fit an
outcome. No read value, original instruction, PC or RAM snapshot is exported.

The final 32-case grid covers both models and live tempos, gaps 8/9/10/11 on SGB1
and 4/5/6/7 on SGB2, and spins 0/1024. A preliminary grid starting one frame later
found only requests after the operand. The final grid extends earlier to test
both sides. Coordinates choose arrival times; no selection-delay outcome fits
parameters. Wait errors are reported against the frozen admitted waits of
20 ms at tempo 128 and 32 ms at tempo 32, with the existing 2-ms tolerance.
Reports explicitly retain `qualification: false` and `parameters_fitted: false`.

```sh
python3 scripts/check_sgb_vendor_tempo_boundary_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

Each case uses 300 million SNES clocks, a 240-second timeout and a 96-KiB output
bound; four immutable fixtures run concurrently. Original execution is opaque,
with only public DSP writes, owned-address timestamps and bounded timer metadata
retained. Failures suppress child diagnostics and partial JSON. Program/fixture
hashes remain in the local report; original images and reports remain outside
public CI.

All 32 final local original captures passed, with exactly one operand read per
case and no timer configuration writes. There were 26 cases with the operand
read before the GB request and six with the request before the read. All reads
preceded the final selected-directory lookup. No case needed the absent or
ambiguous category. Every wait matched its live-tempo prediction within 2 ms.
The unchanged-tempo controls have identical starting/live predictions and are
not evidence for discriminating those hypotheses.

| Changed-tempo case | Count | Signed operand age (ms) | Directory-to-root wait (ms) |
| --- | ---: | ---: | ---: |
| SGB1, request before read | 1 | -16.590332 | 31.998047 |
| SGB2, request before read | 2 | -13.485352 to -13.477051 | 32.407227 |
| SGB1, read before request | 7 | 0.106934 to 49.956543 | 31.998047 to 32.451172 |
| SGB2, read before request | 6 | 3.215820 to 36.500977 | 31.595703 to 32.000977 |

Negative age means the request arrived before the read. The closest positive
SGB1 age was 0.106934 ms, before the new-tempo note; several old-track notes
therefore carried request counter 2. The boundary mode retains those counters
while correctly identifying the distinct selected song. These observations
reject latching only the pre-change tempo at GB delivery in this corpus. They
do not locate the APU sampling instruction, establish arbitrary tempo support,
or qualify complete audio equivalence. No firmware change follows yet.

Public controls validate the same 32-case grid against the unchanged opt-in
firmware, without requiring it to reproduce original startup timing. Four safer
other-schedule pairs compare all earlier metadata outside the interruption window
(excluding the intentionally extended operand-read scope);
four native/scalar pairs compare PCM/timing and check reset, destination load,
unread samples and wait/pulse snapshots. Synthetic contracts cover old-track
notes under request counter 2, reads on either side of delivery, absent/ambiguous
reads, invalid bindings, missing gates and sanitized failures. Both firmware
images remain unchanged; this is a timing experiment, not firmware qualification.

The [handoff correlation](sgb-vendor-tempo-handoff.md) now retains public APU
music-value writes and matching replies from these same coordinates. Every
candidate publication follows the operand read, so earlier GB delivery alone
does not establish the tempo state when the APU command is published.
