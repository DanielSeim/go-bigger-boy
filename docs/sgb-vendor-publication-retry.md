# Direct publication/retry gate and nearby holdouts

The [delayed-tempo experiment](sgb-vendor-delayed-tempo.md) found four SGB2
changed/unchanged pairs with music publication before the authored E7 read.
The changed member has an unreplied publication followed by a replied retry;
the unchanged control has one publication. A root-wait match alone cannot prove
that the original and replacement implement the same handoff.

This follow-up runs the same authored cartridge against both private originals
and the unchanged opt-in `selection_wait=True` replacement, comparing complete
bounded summaries. It adds no probe mode and changes no emulation or firmware.

## Fixed scope

All fixtures start at tempo 128, change after the third note to either 32 or
unchanged 128, and use prefix duration 8. The E7 operand stays at `$2B70`.

| Group | SGB2 gap/spin schedules | SGB1 model controls |
| --- | --- | --- |
| Baseline | `(7,2048)`, `(8,0)`, `(8,512)`, `(8,1024)` | Same spins, gap +4 |
| Holdout | `(7,1792)`, `(8,256)`, `(8,768)`, `(8,1280)` | Same spins, gap +4 |

Cross both models and both live tempos: 16 coordinates per group, 32 total,
64 original/replacement captures. The holdout schedules were frozen before
capture and are absent from the prior 96-case delayed-tempo corpus. Their score
and sample payloads are unchanged. They are **new authored schedules**, not a
promise of distinct physical phases: frame-quantized delivery can collapse
several offsets to the same effective arrival.

## Gate and report

```sh
python3 scripts/check_sgb_vendor_publication_retry_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

Schema `gbb-sgb-vendor-publication-retry-v1` retains `qualification: false` and
`parameters_fitted: false`. Both binaries execute the same fixture image.
The executable and private program images are frozen copies; their hashes,
the generated replacement hash and every fixture hash are recorded. Four
coordinates execute concurrently; each child is bounded to 300 million SNES
clocks, 240 seconds and 96 KiB. Original code remains opaque, and reports contain
only the existing public/owned-address observation metadata. Private inputs and
local reports remain uncommitted.

The gate validates:

- Every coordinate is present exactly once with its exact generated fixture hash.
- Both captures pass the established-playback, binding, cadence, gates,
  directory/traversal ownership, timer-configuration and completion guards.
- Every paired root wait agrees within the previously frozen 2-ms tolerance.
- All eight SGB2 baseline members retain the previously observed original retry
  anchors: changed tempo has exactly two chronological candidates, the first
  before E7 without a matching reply and the second after E7 with a reply;
  unchanged tempo has one pre-E7 candidate with a reply. Both retain the authored
  operand-before-directory ordering and directory-before-matching-reply ordering.

Holdouts are **not required** to have the baseline retry shape. Their outcomes
are reported independently without changing the frozen tolerance or anchors.
The comparison records every candidate, port, matching reply count and ordering
relative to operand/directory reads; it never substitutes a matching reply for
proof of acceptance or picks one candidate to fit the wait.

`gate.passed` means only root-wait agreement plus the frozen original baseline
anchors. `gate.handoff_equivalent` and each `handoff_profile_equal` are separate
observation-profile comparisons, never implied by wait agreement. Even matching
profiles would not qualify waveform, title or instruction-level equivalence.

Each comparison additionally records the difference in old-track age at GB
request 2 (`prior_keyon_age_difference_ms`) and whether old note count and operand
read ownership agree (`prior_score_state_equal`). These are explicit diagnostic
witnesses, not an alignment correction or permission to ignore a failed wait.

Exit 0 means the bounded gate passed. Exit 1 emits the complete report with
`gate.passed: false` for a validly observed behavioral mismatch. Exit 2 means an
invalid/failed capture or corpus; child diagnostics and partial JSON are suppressed.
A failed gate must not be waived or relabeled as successful qualification.

## Validation and integration

Four short test methods cover disjoint holdout scope and identical payloads,
original retry anchors, wait/profile separation, prior-state witnesses, malformed
and duplicate identities, recomputed comparisons, missing cases and sanitized
failures. They also exercise CLI exits 0/1/2 without private inputs.

`gameboy_sgb_score_vendor_publication_retry` runs 32 owned captures, eight
observation-transparency pairs and eight native/scalar reset/save-load pairs,
including active-wait, elapsed-pulse and unread-audio restoration. Its timeout is
900 seconds and cost 450. It enters dedicated Linux firmware shards through the
existing `sgb-firmware-extended` label. Public discovery becomes 99 matrices.

`gameboy_sgb_vendor_publication_retry_local_reference` is the optional private
64-capture gate, timeout 1800 seconds, labels `local;private-reference`. It stays
out of public shards. Existing matrices and firmware image defaults are unchanged.

## Local results and current failure

Both 64-capture original/replacement runs completed. Adding the first-KON age
witness reproduced every earlier capture-summary field exactly. The new private
gate correctly returns **1**, retaining the complete report; it is currently a
failing compatibility gate, not a passing qualification claim.

| Group | Paired wait matches | Maximum wait error | Frozen original anchors |
| --- | ---: | ---: | --- |
| Baseline | 13 / 16 | 12.193 ms | All eight SGB2 members pass |
| Holdout | 12 / 16 | 12.225 ms | Not fitted to baseline retry shape |

All seven mismatches are changed-tempo (32) cases. Original playback switches
without an observed E7 read and waits about 20 ms; the replacement already read
E7 and waits about 32 ms. The failing coordinates are:

| Group | Model | Gap/spin |
| --- | --- | --- |
| Baseline | SGB1 | `(11,2048)`, `(12,0)`, `(12,512)` |
| Holdout | SGB1 | `(11,1792)`, `(12,256)`, `(12,768)` |
| Holdout | SGB2 | `(7,1792)` |

No paired handoff profiles match, and no paired preceding-score-state witnesses
match. The replacement's old track is already 102.659–117.691 ms older at request
2 on SGB1, and 44.241–59.113 ms older on SGB2. These measured first-KON age
differences show why identical GB scheduling inputs do not phase-align prior
playback. They do not identify which startup stage caused the difference or
justify changing a wait constant.

The full owned matrix passes, including both groups, transparency and all eight
native/scalar reset/save-load pairs. All five focused contract CTests pass (new
retry, delayed tempo, earlier publication, selection wait and shard contracts).
Public discovery includes all 99 matrices exactly once and excludes the private
gate. Syntax and whitespace checks pass. Normal and opt-in firmware image hashes
remain unchanged. No production timing, port behavior or default is changed.

The [startup/phase follow-up](sgb-vendor-startup-phase.md) now decomposes
first-selection directory/root/first-KON timing and calibrates fixture request
scheduling using separate unchanged-tempo age witnesses. It independently checks
preceding playback state before comparing waits. These seven same-schedule
failures remain active; the new experiment does not waive them, enlarge their
tolerance or fit a firmware wait table to different starting states.
