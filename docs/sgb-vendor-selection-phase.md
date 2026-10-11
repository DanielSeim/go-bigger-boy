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

Next, vary the first request independently of later spacing and compare actual
directory-read arrival phase across both models. Reserve unseen schedules
before testing a phase-aware candidate. Keep tempo 112 excluded until a
predictor passes the original timing bound and owned interruption/replay gates.
