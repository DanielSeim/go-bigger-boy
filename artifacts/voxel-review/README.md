# Voxel review snapshot

This directory contains an archived snapshot of reviewable output from an
automated voxel-scene corpus run, not current renderer acceptance evidence.
Local metadata review on 2026-10-05 confirmed the counts below against
`run-manifest.json`; captures and analyzer results were not regenerated.

- `index.html` is a self-contained report with the generated scene proposals.
- `proposals/` contains the analyzer's deterministic proposal JSON files.
- `movies/` contains the generated input movies used to explore each ROM.
- `run-manifest.json` records the corpus, capture status, and artifact paths.

The raw per-frame observations were intentionally not copied here. At snapshot
time they were in the ignored `roms/voxel-review/observations/` directory and
the capture set was reported as about 1.8 GB; their present availability and
size are not guaranteed. The report and proposal artifacts are sufficient
for reviewing the generated results without adding that large local dataset to
the synced repository.

This snapshot was generated from the 32-ROM corpus. The run completed 13
entries, recorded 8 partial captures, and reused 11 captures from the earlier
run.

`skipped` records mean existing observation/proposal files were reused, not a
new successful capture. Manifest paths for `observations/` and `logs/` refer to
the original run layout; those directories are not included in this snapshot.
Generated proposals require manual review and do not enable live profile rules
or establish successful gameplay/object recognition across the corpus.
