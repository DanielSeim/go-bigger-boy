# Voxel review snapshot

This directory contains the reviewable output from the automated voxel-scene
corpus run.

- `index.html` is a self-contained report with the generated scene proposals.
- `proposals/` contains the analyzer's deterministic proposal JSON files.
- `movies/` contains the generated input movies used to explore each ROM.
- `run-manifest.json` records the corpus, capture status, and artifact paths.

The raw per-frame observations are intentionally not copied here. They remain
in the ignored `roms/voxel-review/observations/` directory because the current
capture set is about 1.8 GB. The report and proposal artifacts are sufficient
for reviewing the generated results without adding that large local dataset to
the synced repository.

This snapshot was generated from the 32-ROM corpus. The run completed 13
entries, recorded 8 partial captures, and reused 11 captures from the earlier
run.
