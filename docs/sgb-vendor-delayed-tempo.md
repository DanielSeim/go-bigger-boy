# Delayed authored tempo change during established playback

The [earlier publication sweep](sgb-vendor-tempo-publication.md) advanced requests
until they overlapped startup, without finding a publication before an observed
tempo operand. This follow-up delays the authored change instead. It changes no
production firmware, timing policy, bus implementation or probe mode.

## Fixture and observation contract

`build_sgb_vendor_tick_selection_fixture.py --prefix-duration` accepts 2 (the
unchanged default), 4, 8 or 16 ticks in boundary mode. Changing the existing
initial duration byte extends the three pre-change notes without inserting bytes:
the authored E7 operand remains at `$2B70`. The duration also applies to the fourth
old note after E7, until the existing 96-tick tail. The selected song retains its
distinct pitch and duration. Except for that duration byte and the cartridge
checksum, the fixture is unchanged. Existing default fixture hashes are retained.

The sweep fixes 96 coordinates before the full original capture:

| Prefix duration | SGB1 request gaps | SGB2 request gaps | Spins |
| --- | --- | --- | --- |
| 4 | 8, 9, 10 frames | 4, 5, 6 frames | 0, 512, 1024, 2048 |
| 8 | 10, 11, 12 frames | 6, 7, 8 frames | 0, 512, 1024, 2048 |

Every coordinate pairs a changed tempo 128→32 with an unchanged 128→128 control.
The shorter eight-case pilot informed the duration/gap ranges. These schedules
are an exploratory corpus, not an independent holdout or production qualification.
Frame/spin settings are authored scheduling controls; they are not assumed to
produce continuous or exact physical shifts.

The existing `--tempo-publication` observer captures only public DSP events,
fixed owned-score reads, address-only timer metadata and APU equality/reply
metadata. Original execution remains opaque. The checker retains every music
write, repeated-write ambiguity, absent operands, startup ownership, binding,
gates, directory/traversal ownership, timer configuration and completion guards.
The existing continuous-accumulator cadence validator now accepts an explicit
owned duration; its default arithmetic and the earlier sweeps remain unchanged.

```sh
python3 scripts/check_sgb_vendor_delayed_tempo_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The schema is `gbb-sgb-vendor-delayed-tempo-v1`, with `qualification: false` and
`parameters_fitted: false`. Each case runs for 300 million SNES clocks, with a
240-second child timeout and 96-KiB capture limit; four run concurrently. The
checker freezes local copies of its executable and private input images before
running and records their hashes alongside fixture hashes. Failures suppress
child diagnostics and partial JSON. Private inputs and local reports are not
committed.

## Interpreting publication evidence

`unique_publication_crossing` requires established playback, exactly one music
write, one observed operand, publication before that operand, and the operand
before the selected-directory read. It permits the narrow unchanged/changed
comparison without silently choosing among repeated publications.

Separately, `first_publication_before_operand` always uses the chronologically
first music-value write. Its `first_publication_evidence` tests only the fixed
hypothesis "tempo is retained at the first publication," against the earlier
frozen 20-ms starting-tempo and 32-ms changed-tempo predictions, with the existing
2-ms tolerance. It does not identify command acceptance or discard later writes.
Repeated-write cases keep `timing_evidence: not_distinguished` for a unique
handoff even when the narrower first-write hypothesis has useful evidence.

`comparison.distinguishing_pairs` counts pairs with unique crossings in both
members. `first_publication_pairs` counts paired first-write crossings;
`later_tempo_after_first_publication_pairs` additionally requires the changed
case to agree only with the later-tempo prediction and its control to agree with
the starting prediction. Missing operands, ambiguous operands and shared
publication/operand timestamps cannot satisfy these guards.

## Local original results

All 96 original captures pass their observation contracts, with established
playback before every observed music publication and no startup overlap.

| Publication/operand outcome | Cases | Changed-tempo wait | Unchanged-tempo wait |
| --- | ---: | --- | --- |
| Operand not observed after interrupted playback | 60 | 19.598–20.009 ms | 19.598–20.009 ms |
| Every music write follows operand | 28 | 32.001–32.454 ms | 19.558–20.011 ms |
| Writes straddle operand (changed member) | 4 | 31.596 ms | — |
| Unique music write precedes operand (paired control) | 4 | — | 19.598 ms |

The four crossing pairs are SGB2, duration 8, gap/spin `(7,2048)`, `(8,0)`,
`(8,512)` and `(8,1024)`. In both members the first music publication precedes the
owned operand read by 2.274–2.282 ms. For changed tempo, that publication has no
matching reply in its latest-write interval; another music publication follows
the read by 14.293–14.301 ms and does receive a reply. The unchanged control has
one publication and its first matching reply follows the read by 4.318 ms. No
original port byte or instruction trace is exported.

All four paired wait differences are 11.998 ms. Therefore these cases disagree
with retaining tempo at the **first observed music publication**: the changed
member instead agrees with the frozen 32-ms later-tempo prediction, while its
control agrees with 20 ms. They still do not identify the acceptance instruction
or a unique handoff in the changed member. All matching replies in these pairs
occur after the selected-directory read, so they cannot be substituted as a
pre-directory acceptance anchor. There are four first-publication pairs and
zero pairs with unique publication crossings in both members.

The opt-in owned replacement was also captured at all eight crossing coordinates.
Its waits agree with the originals within 0.714 ms, but all its music publications
follow the operand read. This supports preserving the current opt-in wait policy
while investigating the different handoff path. It does not establish equivalent
publication/acknowledgment behavior or justify enabling the experiment by default.
A repeat using the freshly built, frozen probe reproduces all 96 earlier
observation summaries exactly. The full owned matrix passes, including
observation transparency and native/scalar reset/save-load continuation. The
new contract, earlier publication contract, selection-wait contract and shard
contract all pass. Python syntax and whitespace checks pass. The normal DB
image and opt-in selection-wait image retain their prior hashes.

## Validation and next gate

The short contract tests cover unchanged default images, the single duration-byte
change and checksum, fixed operand address, generalized cadence, malformed
controls, both frozen predictions, repeated publications, ambiguous/shared
observations, exact corpus scope and sanitized failures. The public matrix runs
all 96 owned timelines plus eight observation-transparency pairs and eight
native/scalar reset/save-load playback pairs, retaining in-flight wait and unread
buffer restoration. The optional local original gate stays separate from public
CI. Tests are evidence for this bounded observation contract, not sound equivalence.

The next target is a paired behavioral gate for the first unreplied publication
and retry: preserve both candidate writes and their reply intervals, compare
original and replacement at the four crossing schedules, and add separate
nearby holdout schedules before changing acceptance or timing behavior. This is
a specific remaining handoff question, rather than evidence for another fitted
wait-table entry.
