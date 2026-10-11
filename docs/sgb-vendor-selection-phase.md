# Tempo-112 completed-selection phase controls

The [opt-in selection pacing](sgb-vendor-selection-wait.md) excludes tempo 112
because a constant delay failed its independent ±2-ms gate. This follow-up
separates preceding-note duration from later request spacing. It changes no
firmware, production selection or timing bound.

The authored first song uses tempo 112 and duration 24, 48 or 96. Songs 2 and 3
retain tempo 45, duration 96, distinct notes and the same owned uploaded assets.
The first request schedule is fixed; subsequent requests wait 100 through 104
frames. All 30 opaque-original captures pass the existing distinct-note,
instrument, completed-playback, directory/traversal and final-release guards.
Completion is verified by observed key-off before each later request, rather
than assumed from the frame count. Reports export bounded timing and hashes.

| Model | First duration | Second-selection directory-to-root range |
| --- | --- | --- |
| SGB1 | 24 | 20.002930–20.003906 ms |
| SGB1 | 48 | 20.002930–20.003906 ms |
| SGB1 | 96 | 20.002930–20.003906 ms |
| SGB2 | 24 | 15.599609–17.997070 ms |
| SGB2 | 48 | 15.599609–18.004883 ms |
| SGB2 | 96 | 15.599609–18.004883 ms |

At a fixed frame offset, changing duration moves the SGB2 root wait by at most
0.007813 ms; the SGB1 waits are unchanged. By contrast, changing request offset
moves SGB2's root wait by about 2.4 ms. For duration 48, offsets 0..4 yield
16.403320, 18.004883, 15.599609, 17.598633 and 16.403320 ms. The unchanged
tempo-45 third-selection control spans 25.590820–26.413086 ms across all cases.

The first release occurs about 110 ms later at duration 48 than at 24, and
another 220 ms later at 96, without a corresponding shift in the next root
wait. This argues against time since natural completion as the main predictor
in these completed controls. Persistent scheduling phase is a useful next
hypothesis; this experiment does not identify the timer or its reset rules.

Measured request gaps matter: duration-48 SGB1 gaps span approximately
1649.289–1699.085 ms and SGB2 gaps 1666.184–1732.539 ms. Adjacent frame settings
can produce nearly identical delivered gaps on SGB1. Model and arrival phase
remain confounded, so this evidence does not justify a model-specific table.
The aggregate root-wait spread is 4.404297 ms: even an ideal constant cannot
cover all cases within ±2 ms. A feasible interval within a subgroup is merely
a necessary condition for fitting that subgroup, not independent validation.

Reproduce with caller-owned originals:

```sh
python3 scripts/check_sgb_vendor_selection_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

`--duration` and `--request-offset` can repeat and deduplicate controls. Four
captures run concurrently with immutable, distinct fixture files. Each retains
the 260-million-clock, 240-second and 64-KiB bounds. Failures suppress child
diagnostics and produce no partial JSON. Successful reports retain
`qualification: false`; they contain no original instructions, private score
bytes, RAM snapshots or PCM.

The optional CTest `gameboy_sgb_vendor_selection_duration_local_reference`
is labelled `local;private-reference`. Public short contracts test fixture
isolation/checksums, constant-delay infeasibility, completed-traversal guard
propagation, deduplication and sanitized failure. The existing public selection
matrix adds four owned tempo-112 duration-24/96 completed controls, checking
the excluded-point fallback and the admitted tempo-45 next selection. No new
public matrix is introduced. The four owned controls and six short contracts
pass locally; Python compilation and whitespace checks pass.

The independent-arrival follow-up below varies the first request separately
from later spacing and reserves unseen schedules. Tempo 112 stays excluded
until a predictor passes the original timing bound and owned replay gates.

## Independent first-request and subframe arrival controls

The follow-up fixes duration 48 and later spacing at 102 frames. First-request
offsets 0 and 2 combine with selection-2 spin counts 0, 512, 1024, 1536 and 2048
for 20 original captures. Eight held-out captures use first offsets 1 and 3
with spin counts 0 and 1536. All pass the existing completed/traversal guards.
Only authored GB scheduling code changes; score and asset bytes are identical.
The builder's default output is unchanged. Non-normal interruption profiles
reject these extra phase controls.

| Model | First offset | Spin count | Second-selection root wait |
| --- | --- | --- | --- |
| SGB1 | 0 | 0 | 20.002930 ms |
| SGB1 | 0 | 512–2048 | 20.002930 ms |
| SGB1 | 2 | 0 | 20.002930 ms |
| SGB1 | 2 | 512–2048 | 15.599609 ms |
| SGB2 | 0 | 0–512 | 15.599609 ms |
| SGB2 | 0 | 1024–2048 | 17.598633 ms |
| SGB2 | 2 | 0–512 | 16.005859 ms |
| SGB2 | 2 | 1024–2048 | 18.003906 ms |

SGB1 now crosses the same approximately 4.403-ms spread previously observed
between models. With first offset 2, its first request arrives within 0.001 ms
across spin controls, while selection 2 moves from approximately 9768.532 to
9785.313 ms and its root wait drops from 20.003 to 15.600 ms. Model identity
alone cannot explain the variation, and even a separate SGB1 constant fails
the unchanged ±2-ms bound. Held-out SGB1 waits span 15.599609–20.409180 ms;
held-out SGB2 waits span 15.998047–20.002930 ms.

A spin count adds `28*n+8` GB clocks only before SOUND 2; it does not imply that
same increment at the SPC. Observed request timestamps frequently collapse
several spin settings onto nearly the same delivered frame. Absolute half-clock
timestamps in report schema `gbb-sgb-vendor-selection-phase-v2` include first
request, first authored root, second request, last selected-directory read and
second authored root. They come from the existing address-only observer.
No additional original RAM/code access or emulator instrumentation is added.

A tentative 2-ms root cadence fits the first 20 captures to within 0.002 ms
of each model's first measured second-root timestamp. Keeping those anchors
fixed, the held-out SGB1 offset-3/spin-1536 case deviates by 0.405273 ms;
SGB2's held-out maximum residual is 0.002930 ms. Thus the tempting shared
cadence is rejected as a precise general scheduler. A cadence residual measures
timestamp alignment, not prediction of directory-to-root latency or audio.

Reproduce both sets separately, preserving the holdout separation:

```sh
python3 scripts/check_sgb_vendor_selection_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --duration 48 --request-offset 2 --first-request-offset 0 --first-request-offset 2 \
  --selection-spin 0 --selection-spin 512 --selection-spin 1024 \
  --selection-spin 1536 --selection-spin 2048 > /tmp/arrival.json
python3 scripts/check_sgb_vendor_selection_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --duration 48 --request-offset 2 --first-request-offset 1 --first-request-offset 3 \
  --selection-spin 0 --selection-spin 1536 > /tmp/arrival-holdout.json
```

For a model's frozen first-set root timestamp `anchor`, compute each later
root's 2-ms grid residual as
`abs((root_half-anchor+2048) % 4096-2048) / 2048` milliseconds. This is a
diagnostic calculation; no cadence or model-specific wait enters the firmware.
First-offset/spin CLI filters deduplicate controls. Distinct filenames include
all scheduling parameters, preventing concurrent cases from replacing inputs.

The four new owned completed-arrival controls pass. A separate native/scalar
pair on each model with first offset 2/spin 1536 preserves exact PCM, reset and
destination-load continuation, unread buffers and active-wait/pulse restores.
Six short contracts, Python compilation and whitespace checks pass. The full
existing matrix and other platform/sanitizer suites were not rerun locally.
Both default and opt-in firmware hashes remain unchanged; tempo 112 remains
excluded. The [timer-access follow-up](sgb-vendor-selection-timers.md) locates
timer-0 polling inside the variable interval and identifies the next missing
observation: which reads actually consume hardware timer output.
