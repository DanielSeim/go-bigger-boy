# SGB firmware diagnostics in CI

The full public owned-firmware matrices run in the **SGB firmware diagnostics**
workflow on Linux Release builds. Eight independent jobs partition all CTest
tests labelled `sgb-firmware-extended`. Each complete matrix runs once; its
fixtures, variants, lifecycle checks and acoustic assertions are unchanged.

The desktop platform and ASan/UBSan/TSan jobs exclude this label and retain the
shorter contracts, including the shard runner contract. Full matrices therefore
have Linux Release coverage, while shorter contracts retain platform and
sanitizer coverage. This avoids repeating hours of diagnostic playback within
the desktop and sanitizer job limits. The initial Linux run spent over three
minutes on each of its first two acoustic matrices alone.

The label covers native score playback, score upload/instrument/transport
matrices, and full host polyphony, one-shot, instrument-change, stopped/active
bank-replacement/rejection/recovery, one-byte semantic upload tails, audio-transition
and sample-pitch diagnostics. Recovery's two failure types and tail recovery's
single/repeated sequences each run as separate complete matrices. Mixed recovery's
two failure orders, cold recovery's two failure types and cold one-byte recovery's
single/repeated sequences also run as separate complete matrices. Cold mixed
recovery runs each failure order as a separate complete matrix. Fast
fixture generators, scheduler tests and tests ending in `_contract` stay in the
regular suite. Tests labelled `local` or `private-reference` are never included.
Unfiltered local CTest still runs the full suite.

ASan/UBSan jobs allow 45 minutes for their build and shorter contracts. Instrumented
host emulation is substantially slower: the initial ASan run took 391 seconds
for the transfer contract that took 30 seconds in Linux Release. This budget
retains those contracts under sanitizers without shortening their checks.
ThreadSanitizer allows 75 minutes: its firmware contract alone took 33 minutes
in the first run. Desktop jobs allow 60 minutes and build with two workers to
bound concurrent compiler/linker memory use on hosted runners. The initial
macOS ARM build consumed 37 minutes before testing. These budgets retain every
shorter contract; full matrices remain in their separate Linux jobs.

`tests/run_sgb_firmware_shard.py` discovers tests from CTest metadata, balances
their declared timeout budgets deterministically, and builds the probes needed
by the selected shard. It rejects missing targets, empty shards and private
inputs. Anchored, escaped CTest filters select exactly the shard's tests; CTest
failures propagate to the job. Jobs run two tests in parallel, have a 90-minute
limit, and continue independently after another shard fails.

Reproduce one shard locally:

```sh
cmake -S . -B build-firmware -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_BUILD_PERFORMANCE_TESTS=OFF
python3 tests/run_sgb_firmware_shard.py --build-dir build-firmware \
  --shard-count 8 --shard-index 0 --parallel 2
```

Use indices 0 through 7 for the whole suite. Add `--plan-only` to inspect the
partition without building or running probes. Each job uploads its complete
partition plan, JUnit report and CTest log, using public owned inputs only.

To reproduce the shorter CI suite after building all targets:

```sh
ctest --test-dir build-firmware --label-exclude sgb-firmware-extended \
  --output-on-failure
```
