# Controlled SOUND fields and music pitch setup

The [SPC reply experiment](sgb-vendor-reply.md) separates reply publication
from host polling, but equal readback alone cannot identify the exchange that
prepares a selected song. This experiment adds controlled SOUND-field equality
and a distinct-note DSP pitch-setup marker. No firmware, production selection,
emulator timing or DSP arithmetic changes. Performance optimization remains
deferred.

## Authored controls

Eight fixtures retain the preceding distinct-ID bank: music IDs 1, 2, 3
select notes 36, 35, 34 at pitch words 1437, 1357, 1281. The uploaded BRR,
instrument descriptor, tuning 688, tempo 45, duration 96 and articulation 125
stay fixed. Active requests are sixteen GB frames apart; completed requests
are 100 frames apart.

Four profiles change only the effect fields: both zero (`base`), effect A
STOP (`stop-a`), effect B STOP (`stop-b`), or both STOP (`stop-both`). STOP is
128; attributes remain zero. No fixture starts an effect. The baseline images
are byte-identical to the preceding distinct-ID fixtures, with frozen hashes.
Every note must still key on logical channel 2 with its intended pitch and
uploaded source/envelope; extra effect key-ons fail the existing capture gate.

## Observation contract

The optional timeline mode `--selection-handoff` includes the bounded host
and SPC reply observations, plus two arrays:

- `handoff_writes` parallels each host-write record. `fields` is an equality
  mask: bit 0 matches the delivered music ID, bit 1 effect A, bit 2 effect B,
  and bit 3 attributes. When controlled fields have the same value, all their
  bits must appear together. Unknown values receive mask zero. The record
  also counts SPC publications equal to the controlled music ID and records
  their first timestamp within that host-write interval.
- `selection_pitch_writes` records channel-2 DSP pitch-port writes before each
  request's first key-on, with request, physical half-clock and resulting pitch
  word. The checker finds the first setup of that request's distinct expected
  pitch. This is a physical DSP setup marker, not an inferred firmware PC,
  private score cursor or instruction-level commit.

The mask exposes comparisons against the authored request, never private port
bytes. Coincidental equality remains possible, including protocol/status
values equal to a music ID. The nearest music-value write before pitch setup
is therefore reported as a candidate, with no requirement that one exists and
no claim that it alone commits selection. Missing candidates remain null.

The checker retains all preceding setup, request ownership, active/completed
state and final release contracts. It rejects contradictory equality masks,
record mismatches, out-of-window publications, inconsistent overlapping or
disjoint reply counts, unexpected fields, unordered/out-of-window pitch writes
and missing expected pitch setup. Same-value music and host-write comparisons
must have identical counts and first timestamps. Distinct values cannot
claim overlapping publication counts.

The limits remain sixteen requests, 128 host-write records, 128 changed DSP
events and a million read/reply observations per record; pitch setup adds a
separate 128-write cap. Overflow fails without a partial success report. The
private capture retains a 260-million-master-clock bound and 240-second timeout;
this opt-in mode has a 48-KiB stdout bound, while preceding modes retain 32 KiB.
Only bounded register/timing metadata, equality classes and image identities
are exported. No firmware instructions, RAM, raw packets, tables, assets,
snapshots or PCM are committed or exported by the private checker.

## Reproduce

```sh
cmake --build build-dmg-firmware --target \
  gameboy_sgb_vendor_music_probe gameboy_sgb_music_timeline_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_handoff_contract|score_vendor_handoff)$' \
  --output-on-failure
python3 scripts/check_sgb_vendor_handoff_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The private checker supports model/profile/state restrictions. It executes
caller-owned originals opaquely with the same native host and bundled GB/SPC
bootstraps as the replacement. Its schema is `gbb-sgb-vendor-handoff-v1` and
`qualification` remains false. Successful observation does not qualify timing
compatibility, acoustics, an independent emulator or physical hardware.

The public matrix runs all eight fixtures on both models, compares ordinary
timeline output with and without handoff collection, and retains four exact
PCM/state reset/save-load replays of the both-STOP control. The short fixture
and report contract stays in platform/sanitizer jobs; the full matrix runs in
the dedicated firmware shards.

The DB firmware remains SHA-256
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Production still requires SGB1/SGB2 program ROMs. Continuous score phase,
the unmatched short gate, multichannel scores, effects, fades and acoustic
qualification remain separate work.

## Private findings, 2026-10-11

All 32 fresh captures complete: eight fixtures, both models and both original
and replacement programs. Every distinct pitch/source/envelope, preceding
active/completed state and final release passes. Original program identities
match the preceding reply experiment; the DB image remains unchanged.

Effect-STOP controls change original key-on delays by at most 0.015 ms on
SGB1 and 0.020 ms on SGB2 relative to the corresponding baseline. Pitch setup
precedes original key-on by 1.043..1.080 ms across all controls and both models;
the replacement interval is 0.217..0.230 ms. These outward-rounded intervals
put most of the observed selection delay before pitch setup.

Baseline original pitch-setup delay after delivered SOUND:

| Model/state | ID 1 | ID 2 | ID 3 |
| --- | --- | --- | --- |
| SGB1 active | 128.032 ms | 43.060 ms | 59.888 ms |
| SGB1 completed | 128.033 ms | 64.734 ms | 65.902 ms |
| SGB2 active | 58.368 ms | 79.042 ms | 66.095 ms |
| SGB2 completed | 58.369 ms | 68.170 ms | 65.726 ms |

The field controls supply a counterexample to a last-matching-write rule.
For SGB2's first baseline selection, the last port-0 write matching music ID 1
is near 16.65 ms. In the effect-STOP controls, a later port-0 write near
33.22 ms also matches ID 1. Pitch setup remains near 58.37 ms and key-on near
59.42 ms. The candidate moves by about one frame without a corresponding
selection-time change. Repeated equal values therefore cannot identify
which write commits the selection.

The authored replacement provides an additional known-protocol control:
music is sent on port 2, but its token on port 0 and command kind on port 3
can equal an ID too. The reported last candidate for ID 2 is port 3 rather
than the actual music field on port 2. The report deliberately retains that
coincidence instead of relabeling the candidate as command acceptance.

These observations constrain the handoff but do not yet identify a unique
commit exchange. Neither reply equality nor proximity to DSP setup justifies
adding an arbitrary delay to the replacement.

## Next step

Observe native SPC reads of the fixed, independently authored score directory
at `$2B00..2B05`, exporting only addresses and timestamps. Correlate actual
selected-root reads with the classified host exchanges, reply publication
and pitch setup. Keep validation and live playback reads distinct where the
replacement traverses the directory more than once. That selection-specific
marker should narrow the acceptance boundary without relying on coincidental
port values. Establish that boundary before implementing dispatch pacing;
held-out phase, rapid reselection, STOP/upload ownership and pinned-title
validation remain required for any eventual policy.

## Validation evidence

All three public test methods pass in 315.792 seconds, including sixteen
handoff captures, sixteen ordinary-timeline controls and four exact reset/load
PCM/state replay scenarios. Four focused observer, dispatch, reply and handoff
CTests pass in 3.74 seconds. Four shard-runner tests pass; discovery registers
the handoff matrix exactly once among 90 public matrices with both probe
targets. The baseline fixture hashes, native probe build, Python compilation
and whitespace checks pass.

All 32 fresh private captures pass. No original program instructions, resident
assets or proprietary score bytes were implementation inputs. Earlier full
firmware matrices, pinned private title playback, Windows execution,
independent emulators and physical hardware were not rerun. The firmware
image, production selection and performance policy remain unchanged.
