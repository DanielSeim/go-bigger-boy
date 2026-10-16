# First-selection startup and request-phase comparison

The [publication/retry gate](sgb-vendor-publication-retry.md) exposed seven
roughly 12-ms wait mismatches with different preceding score states. This
follow-up decomposes startup and adjusts **fixture scheduling**, then requires
independent preceding-playback witnesses before comparing waits. The previous
same-schedule gate and all seven failures remain unchanged.

## Startup anchors

The existing bounded observer supplies request 1, the last first-song directory
read (`$2B00/$2B01`), first root read (`$2B10/$2B11`) and first old-song KON.
The checker retains all earlier sequence, binding, cadence, gate, ownership,
timer and completion guards, and requires chronological startup anchors.
It reports request→directory, directory→root, root→KON and request→KON intervals.

Request spacing minus request→KON is the old-track age at request 2. Keeping
both terms makes it possible to distinguish startup latency from different GB
request delivery spacing. No original code, tables, samples or RAM values are
inspected; original execution remains opaque.

## Scheduling calibration and independent witnesses

Each model has three dedicated unchanged-tempo (128→128) calibration captures:
original at gap 12 for SGB1 / gap 8 for SGB2, replacement at the same gap, and
replacement at one frame earlier. All use prefix duration 8 and spin zero.

The replacement age difference between adjacent fixture gaps supplies an
observed one-frame step (required to be 10–25 ms). Round the original/replacement
age difference to the nearest such step to obtain a single integer gap reduction
per model. Bounds are 0–9 frames and resulting fixture gaps 3–12; unsupported
reductions fail rather than clamp. Only first-KON ages enter calibration: **no
wait times, changed-tempo results, replies or holdouts may choose the reduction**.

Apply that fixed reduction to all 32 previous baseline/holdout coordinates,
retaining their spin, score and samples. Original and replacement fixture hashes
are distinct and validated; their complete payload regions must remain identical.
The original cartridge schedule is unchanged. This is a separate experimental
comparison and cannot make the earlier same-schedule regression pass.

A pair is `phase_aligned` only if:

- Old-track ages at request 2 agree within the frozen 2-ms tolerance.
- The pitches of all notes starting before request 2 agree.
- Both captures agree whether the unique authored E7 operand was read before
  request 2. Multiple operand reads or a note/read sharing the request timestamp
  make the boundary ambiguous and prevent alignment.

`phase_controlled_wait_agreement` is `null` for unaligned pairs, even when their
waits happen to match. Otherwise it retains the existing 2-ms wait check. This
is a bounded witness definition, not proof of equal fractional music-tick phase,
internal acceptance state or cycle-exact equivalence. All original/replacement
handoff candidates remain in the report.

## Running and interpreting the gate

```sh
python3 scripts/check_sgb_vendor_startup_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

Schema `gbb-sgb-vendor-startup-phase-v1` explicitly records `qualification: false`,
`parameters_fitted: true` and `fitted_parameters: ["replacement_request_gap_frames"]`.
These are fitted **test scheduling** parameters; no emulator or firmware timing
parameter is changed. Holdout request offsets do not participate in calibration;
authored offsets still need not represent distinct physical delivery phases.

The executable and private program images are frozen and hashed before capture.
There are six calibration captures and 64 paired captures. Two models calibrate
concurrently, then four test coordinates run concurrently. Each child retains
the 300-million-SNES-clock, 240-second and 96-KiB bounds. Private input images
and reports stay local.

Exit 0 requires every pair to pass both phase witnesses and wait agreement.
Exit 1 retains the complete negative report when phase coverage or aligned waits
fail. Exit 2 suppresses child details and partial reports for invalid captures,
calibration, identities or corpus. Neither an unaligned timing match nor zero
aligned cases can become a passing gate.

## Tests and CI

Four short methods test startup decomposition and its age identity, ambiguous
boundaries, bounded age-only calibration, unchanged payloads, strict fixture
identities, independent phase guards, recomputed comparisons, fitted-parameter
disclosure and CLI exits 0/1/2. Calibration tests deliberately corrupt unrelated
wait metadata to confirm it cannot influence the fitted request reduction.

The separate public `gameboy_sgb_score_vendor_startup_phase` matrix exercises
32 adjusted owned fixtures with reductions 7/3, eight observation-transparency
pairs and eight native/scalar reset/save-load pairs, including in-flight wait,
elapsed-pulse and unread-audio restoration. Timeout is 900 seconds, cost 450.
Public shard discovery reaches 100 matrices. The optional private
`gameboy_sgb_vendor_startup_phase_local_reference` has timeout 1800 and labels
`local;private-reference`; it cannot enter public CI shards.

## Local evidence and next target

All 70 captures complete, including a final repeat with shared-timestamp ambiguity
guards that reproduces the earlier paired summaries and gate outcomes. Calibration
selects reductions 7 for SGB1 and 3 for SGB2; observed one-frame steps are 16.344 ms
and 16.742 ms respectively. These measured fixture steps are not new clock constants.

The unchanged-tempo calibration captures decompose startup as follows:

| Model / program | Request→directory | Directory→root | Root→KON | Request→KON |
| --- | ---: | ---: | ---: | ---: |
| SGB1 original | 77.559 ms | 48.002 ms | 3.516 ms | 129.077 ms |
| SGB1 replacement | 6.998 ms | 0.158 ms | 5.767 ms | 12.923 ms |
| SGB2 original | 28.302 ms | 27.594 ms | 3.516 ms | 59.411 ms |
| SGB2 replacement | 6.993 ms | 0.158 ms | 5.767 ms | 12.918 ms |

These are bounded observations for these calibration schedules, not universal
cold-start delays or reconstructed original implementation rules.

| Group | Pairs passing phase witnesses | Aligned wait mismatches | Maximum age error across all pairs |
| --- | ---: | ---: | ---: |
| Baseline | 6 / 16 | 1 | 11.792 ms |
| Holdout | 4 / 16 | 1 | 10.116 ms |

Eight of the ten witness-aligned pairs match waits; 22 pairs remain explicitly
inconclusive. The gate correctly returns 1 for incomplete phase coverage and the
two aligned mismatches. The residual cases are SGB2 changed tempo, original gap
8/spin 512 (baseline) and gap 8/spin 256 (holdout), both with replacement gap 5.
Both have three old notes started and no observed E7 read before GB request 2;
first-KON age errors are 0.513 and 1.216 ms. Original playback subsequently reads
E7 while the request is pending and waits 31.596 ms. Replacement playback switches
without that read and waits 20.280 / 20.249 ms, errors 11.315 / 11.347 ms.

In both cases the first original music-equal write follows GB delivery by
16.646 ms, and its later replied candidate follows at 33.221 ms. Replacement
music-equal writes occur at about 0.133–0.136 ms. The selected-directory reads
follow GB delivery at 35.180–35.183 ms originally, versus 6.409–6.426 ms in the
replacement. All candidates retain their actual ports and equality/reply metadata;
the distinct experimental mailbox protocol must not be relabeled as the original
protocol or assumed to share an acceptance point.

The 32-fixture owned matrix passes, including eight transparency and eight
native/scalar reset/save-load pairs. All six focused contract CTests pass;
public discovery covers 100 matrices once and excludes private gates. Syntax,
whitespace and unchanged normal/opt-in image hashes are verified. The existing
same-schedule publication/retry failures remain active. No firmware, clock,
command-acceptance or default-image behavior is changed.

Next, isolate the GB SOUND delivery→APU staging/commit dispatch path using the
observed per-candidate delays and existing mailbox ownership controls. Test a
separate opt-in dispatch experiment against these two witness-aligned failures,
prior baseline/holdout cases and pending-command reset/save-load behavior before
altering the driver stop point or wait table. First-KON witnesses alone do not
prove identical fractional tick phase, so a dispatch hypothesis still requires
independent checks rather than inserting a fitted one-frame delay globally.
