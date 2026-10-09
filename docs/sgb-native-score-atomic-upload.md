# Atomic owned score/sample uploads

The opt-in D4 diagnostic extends [D3 relocation](sgb-native-score-relocated.md)
with complete-object preflight and invalidation when the SPC enters IPL loading.
It prevents a replacement score from reusing a previous generation's sample
object, or a replacement sample object from reusing the previous score.

Qualification and production playback remain false. SGB1/SGB2 program ROMs
remain required. This is an owned two-object format, not a vendor-bank loader.
Performance optimization remains deferred.

## Build

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --atomic-upload --output /tmp/atomic-host.rom
python3 scripts/build_sgb_score_atomic_fixture.py --kind interleave --active \
  --output /tmp/atomic-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_atomic_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

The image SHA-256 is
`f915a07c9276f5ba269871b4e0ae5e699afb20cf18256dd6b58bb3766f25139f`.
All eight preceding flags are required. Exporters refuse overwrite. Previous
CB/CC/CD/CE/CF/D0/D1/D2/D3 images and the bundled prototype retain their hashes.
The SPC payload is 6386 bytes, ending at `$1AF1`, within `$0200..1BFF`.
The native engine region, 2048-byte score bank, 192-byte asset object and
4096-byte physical SOU_TRN frame retain their existing bounds.

## Transaction contract

The host captures an actual complete GB frame and checks generic list framing,
source/destination bounds and wrap rules before releasing the SPC to IPL.
D4 additionally checks two independent 16-bit coverage cursors:

| Object | Required complete range | Initial cursor | Required final cursor |
| --- | --- | --- | --- |
| Score | `$2B00..32FF` | `$2B00` | `$3300` |
| Sample/descriptors | `$5000..50BF` | `$5000` | `$50C0` |

Each nonempty chunk must start exactly at its object's cursor and finish within
that object's fixed range. The cursor then advances by the chunk length.
Chunks for the two objects may interleave, and either object may arrive first.
Within each object, gaps, overlaps, repeated bytes and backward order reject.
Even omitted zero padding rejects: completeness does not depend on old RAM
contents or on whether a missing byte would happen to validate as zero.
Writes into code, cache, archive, other data or outside these objects reject.
The sole terminal entry is the existing `$0400` restart. Both final cursors
must match before any ownership handoff or data byte upload.

When an accepted list releases ownership, the SPC saves interruption counters,
mutes the DSP, clears staged selection/readiness/root observations, and zeros
all 2048 score bytes and all 192 asset bytes before entering IPL. Uploaded bytes
therefore cannot combine with any previous score/sample bytes. Restart remains
unpublished while all three song roots and the sample object validate. Only the
final directory admission publishes readiness for the new generation.

Archive byte `$0504` records 0 while clearing, `$A4` after clearing and before
publication, and `$A5` after all roots admit. It does not authorize an upload or
bypass validation. It is outside the two admitted destination ranges and the
existing four archived interruption counters.

## Failure and reset behavior

An incomplete or unsafe list fails preflight before ownership release. The host
sends the existing cooperative stop, records error 1 and halts. The previous
complete objects remain intact but muted; later SOUND packets cannot adopt or
play them. No replacement transfer/adoption is counted. D4 captures final
mailbox diagnostics after the stop acknowledgment so row-capture scratch values
do not appear as firmware version/status in the final error report.

A complete list with invalid object contents passes transfer framing, clears
the old generation, uploads the new objects, and fails semantic admission.
The SPC stays muted with zero admitted roots and `$A4` unpublished status.
The host cannot adopt it or play a fallback from the old generation.

Failures remain terminal for this diagnostic run. Cold reset replays bootstrap
and upload from the start; it does not restore a previous generation's readiness.
There is no rollback, resumable transfer or retry after rejection in this profile.
These are readiness/ownership guarantees for the bounded owned format, not
power-loss persistence or a hardware qualification claim.

## Evidence

The owned cartridge changes actual GB VRAM with LCD disabled in VBlank, then
sends SOU_TRN for a different payload. It stores at most three physical payloads
in its 32 KiB ROM and emits at most eight bounded commands. The first generation
uses looping source 2 on both voices; the replacement changes score controls,
source modes, BRR filters, tuning/envelopes, start/loop words and sample bytes.
A third generation changes them back using an interleaved chunk list.

Eight tests cover 90 physical model/mode/scenario runs, each compared across
uninterrupted execution, snapshot restore and cold reset:

- Two D3 regression witnesses show the prior defect: a replacement missing the
  asset object adopts a new score with the previous samples. D3 remains frozen;
  D4 rejects the same transaction before ownership release.
- Complete cold uploads and changed active replacements in full, split,
  reverse-object and interleaved forms, on both SGB models. Active replacement
  also covers native, scalar and combined rendering, with positive ENVX and a
  bounded interruption inside the first 32-tick pattern.
- Stop/reupload in all rendering modes and three distinct generations per model.
- Seventeen incomplete/unsafe replacement cases per model: absent objects,
  directory/descriptor-only objects, short tails, gaps, overlaps, backward chunks,
  code/cache writes, object overflow, wrong restart and malformed framing.
- Four incomplete cold-upload cases per model, with no objects admitted or PCM.
- Complete malformed replacements in all rendering modes, with old data cleared,
  new bytes present, zero admitted roots and no readiness publication.

The read-only probe observes both entire data regions zero after invalidation,
and records the cleared/readiness state once per generation. Final FNV-1a hashes
cover every score and asset byte, proving changed objects replace the earlier
contents completely. Archive phases participate in snapshot selection; metadata,
PCM and full serialized state must match exactly across reset/restore replay.
Native/scalar PCM matches exactly; combined output retains bounded frame counts.
Execution remains capped at 140 million clocks, 4096 restorations, 250000 PCM
frames and 4096 output bytes, with each child bounded to 90 seconds. The new
matrix uses at most 125 million clocks.

All twenty-one selected atomic-upload, relocation, BRR-profile, one-shot,
instrument, directory, transport, host, transfer, firmware, fixture, native-engine
and BRR/DSP contract CTest suites pass (1597.67 seconds total). The new atomic
suite passes in 791.89 seconds. The preceding profiles retain 890 physical
model/mode/scenario checks with the expanded probe. The 370 relocation and 60
BRR-profile arithmetic objects also pass their independent decoder comparisons.
CLI byte reproducibility and overwrite protection, previous image hashes, the
bundled prototype check and `git diff --check` pass.
No proprietary firmware, bank, sample, descriptor or reference PCM is used.
Original-firmware, original-decoder, hardware, acoustic and title qualification
remain outstanding. Production firmware selection and external overrides are
unchanged.

## Next step

Add bounded recovery after upload rejection: keep SOUND unavailable until a
later complete generation validates, while allowing a fresh upload without a
full host reset. Cover both preflight rejection and semantic failure, repeated
retries, stop and save/load. Then widen bank support using measured title demand.
