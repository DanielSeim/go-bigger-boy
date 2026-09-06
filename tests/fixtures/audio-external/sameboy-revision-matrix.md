# SameBoy revision matrix review

This review uses the immutable SameBoy v1.0.3 commit recorded in
[`sameboy-reference-pin.json`](sameboy-reference-pin.json):
`208ba4afabffab9edde416f2dbb8ae459e34adb8`.

The machine-readable reviewed boundary results are in
[`sameboy-revision-boundaries.txt`](sameboy-revision-boundaries.txt).

The fixture producer was run at 96 kHz for the pulse, wave, and noise register
sequences, then converted with `--downsample 2 --start-frame 16` and
`comparison=normalized`. The matrix covered:

| GBB label | SameBoy model | Result |
| --- | --- | --- |
| `dmg` (DMG-B) | `GB_MODEL_DMG_B` | captured |
| `mgb` | `GB_MODEL_MGB` | captured |
| `cgb0` | `GB_MODEL_CGB_0` | captured |
| `cgb-c` | `GB_MODEL_CGB_C` | captured |
| `cgb-e` | `GB_MODEL_CGB_E` | captured |
| `dmg0` | unavailable in SameBoy v1.0.3 | covered by GBB's DMG0 contract |

Three independent takes of all three simple startup fixtures produced the same
normalized signatures across this matrix; each aggregate measured zero
inter-take variation (the recorded `max_abs_error=3`/`rms_error=3` is only the
requested three-unit review margin). That is expected: these sequences do not perform the
revision-sensitive operations (mid-envelope NRx2 writes, divider-aligned
retriggering, PCM reads during a startup window, or NR43 writes at an LFSR
reload boundary). No external waveform files were added based on this result;
promoting an insensitive capture would create a false sense of coverage.

The reviewed, reproducible revision difference is the CGB-D/E NRx2 envelope
write path (“zombie mode”). GBB now models it for `cgb-e` and keeps the
pre-modern path for `cgb0`/`cgb-c`; it is locked by
`test_cgb_revision_envelope_write_behavior` in the APU hardware contract.

The dedicated public-register boundary probes also reproduced these revision
differences in all three takes:

| Fixture | CGB-0/CGB-C | CGB-E | GBB contract |
| --- | --- | --- | --- |
| Square retrigger alignment | first `PCM12` high at step 42 | step 41 | CGB-E is four clocks earlier |
| Channel-4 startup/LFSR | first `PCM34` high at step 15 | step 14 | early CGB adds four startup clocks |
| PCM34 read visibility | live read after step 15 | live read after step 14 | all CGB revisions expose live PCM |
| NR43 reload write (step 5) | first post-write high at step 20 | step 23 | boundary retained as diagnostic fixture |

The first three rows are locked by focused GBB tests. The NR43 reload-write
row is retained for investigation: SameBoy documents revision- and instance-
specific LFSR glitches, so it is not promoted to a behavioral assertion until
we have a deterministic hardware-independent rule rather than overfitting one
emulator instance.

To repeat the capture matrix after changing the pinned SameBoy build:

```sh
for model in dmg mgb cgb0 cgb-c cgb-e; do
  for fixture in pulse wave noise; do
    /tmp/sameboy-audio-capture "$model" "$fixture" "/tmp/$model-$fixture-96k.wav"
    python3 scripts/audio_reference.py convert \
      "/tmp/$model-$fixture-96k.wav" "/tmp/$model-$fixture.txt" \
      --name "$fixture" --model "$model" --source trusted-emulator \
      --comparison normalized --downsample 2 --start-frame 16 \
      --provenance "SameBoy <pinned commit>, $model, $fixture"
  done
done
```

The register-boundary probe can be built alongside the WAV capture helper:

```sh
cc -std=c11 -I/path/to/SameBoy -I/path/to/SameBoy/Core \
  scripts/sameboy_revision_probe.c \
  -L/path/to/SameBoy/build/lib -lsameboy -lm -ldl \
  -Wl,-rpath,/path/to/SameBoy/build/lib -o /tmp/sameboy-revision-probe
/tmp/sameboy-revision-probe cgb-c alignment /tmp/cgb-c-alignment.txt
```
