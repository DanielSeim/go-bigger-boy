# Request-frame phase and score-root consumption

The [traversal experiment](sgb-vendor-traversal.md) localizes a 25–48 ms wait
between selected directory lookup and the first root read on the original
programs. This experiment varies request timing while preserving score and
assets. It measures the response without implementing a polling or pacing law.
Firmware images, production selection and performance policy stay unchanged.

## Authored controls

Five schedules vary one axis at a time:

| Schedule | First SOUND wait | Later waits, active / completed |
| --- | ---: | ---: |
| `baseline` | 16 frames | 16 / 100 frames |
| `first-1` | 17 frames | 16 / 100 frames |
| `first-2` | 18 frames | 16 / 100 frames |
| `spacing-1` | 16 frames | 17 / 101 frames |
| `spacing-2` | 16 frames | 18 / 102 frames |

Each schedule runs distinct IDs 1, 2, 3 in active and completed states with
zero effects (`base`) and both effect fields STOP (`stop-both`). This gives
twenty fixtures, forty original captures and forty replacement captures across
both models. Separate A/B STOP controls were covered by the preceding matrix;
this sweep retains the combined STOP contrast. Offsets of three or more frames
remain unmeasured for future independent validation. No predictive timing
policy is fitted or qualified using these cases.

The existing dispatch builder accepts an optional, bounded three-element tuple
of request frame counts. Its defaults remain unchanged. The baseline images
are byte-identical to the handoff fixtures; every new fixture retains the same
uploaded score/data bytes. Only the authored GB wait code changes. The helper
waits for VBlank transitions; these are frame-count controls, not exact SPC
bus-phase controls.

## Reports and bounds

The checker reuses `--selection-traversal` and every preceding note, envelope,
state, directory, traversal and final-release gate. Each case reports authored
frame counts, measured request gaps, traversal observations, and per-selection
deltas from the same model/profile/state baseline. The first request and later
request spacing are separate axes, so the initial-selection anomaly stays
visible. Measured times retain the existing 2048-half-clock/ms scale; authored
frame counts are never substituted for physical timestamps.

Reports use `gbb-sgb-vendor-request-phase-v1` with `qualification: false` and
fixture/program identities. Baseline captures are included even when a CLI
schedule filter selects another schedule. Per-capture bounds remain sixteen
requests, 128 DSP events, 128 directory reads, 512 traversal reads, 64-KiB
stdout, 260 million master clocks and 240 seconds. No new native observer,
private byte readout, PC trace, RAM snapshot or PCM export is introduced.
Original program images are executed opaquely.

## Reproduce

```sh
python3 tests/sgb_vendor_request_phase_tests.py \
  --timeline-probe build-dmg-firmware/gameboy_sgb_music_timeline_probe
python3 scripts/check_sgb_vendor_request_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe \
  --firmware-dir roms
```

The public full matrix compares forty observed owned timelines against forty
ordinary controls. The short contract checks deterministic fixtures, baseline
identity, score/asset preservation, bounded frame inputs, exclusive export,
summary deltas and retained malformed-traversal rejection. This workload does
not establish commercial sound-bank, acoustic or hardware equivalence.

## Local findings

All eighty private captures pass, with correct pitches, envelopes, request
ownership, active/completed state and final release. The sixteen zero-offset
captures preserve every shared measurement from the preceding traversal run.

| Original model | First-selection directory-to-root wait | Later-selection directory-to-root wait |
| --- | ---: | ---: |
| SGB1 | 47.994–48.004 ms | 25.466–28.011 ms |
| SGB2 | 26.003–27.600 ms | 25.516–28.414 ms |

The replacement's corresponding interval stays 0.131 ms in every case. The
largest combined-STOP movement of the original directory-to-root interval is
0.084 ms on SGB1 and 0.086 ms on SGB2; replacement movement is zero. These are
ranges over this authored workload, not universal tolerance limits.

Frame-count changes do not produce uniform physical delivery increments. For
example, SGB2's active baseline request gaps are 267.311/267.350 ms; adding one
frame to both later waits gives 299.227/268.762 ms, and adding two gives
299.227/302.041 ms. The report retains those actual delivered-request gaps.
Do not treat the authored frame offsets as evenly spaced SPC command arrivals
or infer a bus-phase period from them.

The root wait remains relatively narrow for later selections despite these
arrival changes. SGB1's approximately 48-ms first-selection wait persists
across both first-delay offsets and both STOP controls, distinct from later
selections. This localizes the initial-state difference further but does not
explain it. Neither a constant end-to-end SOUND delay nor a polling-period
formula is justified by this sweep.

Next, vary the *preceding* song's authored tempo while keeping the newly
selected song's tempo, note and assets fixed. Its tempo command is consumed
before the next selection, whereas the selected track's command is consumed
after the root wait being measured. That control can test whether playback
state affects the wait, without presuming its cause. Include currently
unmeasured three-frame offsets and retain first/active/completed and STOP
controls. Any timing policy still needs independent held-out, rapid-selection,
STOP/upload ownership and pinned-title validation before adoption.

## Validation evidence

The public fixture/report/playback run passes three methods in 593.219 seconds:
forty observed timelines, forty identical ordinary controls, deterministic
fixtures and summary guards. The subsequently added filtered-runner contract
also passes using authored metadata only. Nine focused native/report CTests
pass in 5.88 seconds; four shard-runner tests pass. Discovery includes the new
full matrix exactly once among 93 public matrices with its timeline-probe
target. Python compilation and whitespace checks pass.

The unchanged deterministic DB image is
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Original identities remain SGB1
`a75160f7b89b1f0e20fd2f6441bb86285c7378db5035ef6885485eaff6059376`
and SGB2
`c172498a23d1176672931bab33b629c7d28f914a43dca9e540b8af1b37ccf2c6`.
Original instructions, proprietary score bytes and assets were not read as
implementation inputs. Broad earlier matrices, pinned-title playback, Windows,
independent emulators and hardware were not rerun. Performance work stays
deferred and firmware selection stays unchanged.

Follow-up: [preceding-song tempo controls](sgb-vendor-prior-tempo.md) now show
that tempo 96 shortens the next selection's directory-to-root wait relative
to tempo 45, including completed playback and the three-frame spacing offset.
Bounded transition volume-write observations are next; pacing stays unchanged.
