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
recovery, with whole sample chunks or a one-byte semantic tail, runs each
failure order as a separate complete matrix. Staggered subframe SOUND commands
add a complete matrix for each cold mixed one-byte failure order. The separate
vendor-format music matrix also runs here using owned inputs; its optional
private title gate stays outside public shards. Fast
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

The uploaded instrument-2 extension keeps the DB full matrix in the firmware
shards. `gameboy_sgb_music_observer_contract` is a short public lifecycle check;
`gameboy_sgb_vendor_music_local_reference` requires caller-owned originals and is
labelled `local;private-reference`, alongside the optional private title gate.
The reference probe exports bounded register/timestamp metadata only.

The DB matrix also executes owned timing/release fixtures and a bounded register
probe (`--timeline-probe`). Its cost is 240 and timeout 900 seconds; the optional
private gate adds `--timing` for exact uploaded pitch and bounded gate/release
comparisons. Both probes are discovered as shard build dependencies.

The uploaded-score phase suite is a separate `gameboy_sgb_score_vendor_phase`
full matrix (cost 300, timeout 900 seconds), bringing the public total to 86.
It uses owned longer chains and repeated selections on both models, including
reset/save/load replay of the long-note/rest sequence. Its fixture/report
contract remains in ordinary platform/sanitizer jobs. The optional
`gameboy_sgb_vendor_phase_local_reference` reports measured timing gaps and is
labelled `local;private-reference`; successful observation does not qualify
compatibility. See [the phase experiments](sgb-vendor-phase.md).

The selection-isolation suite adds `gameboy_sgb_score_vendor_selection` (cost
300, timeout 900 seconds), bringing the public total to 87 full matrices. It
covers fixed-note prefix/tail workloads and subframe requests in active and
completed states. Its short fixture/port-summary contract and the combined
DSP/APU observer lifecycle contract retain platform/sanitizer coverage. The
optional selection reference experiment is labelled `local;private-reference`.
See [selection workload and port timing](sgb-vendor-selection.md).

The controlled dispatch suite adds `gameboy_sgb_score_vendor_dispatch` (cost
300, timeout 900 seconds), bringing the public total to 88 full matrices.
It covers distinct and repeated music IDs in active/completed states on both
models, unchanged ordinary timelines under readback collection, and exact
reset/save-load replay. Its short fixture/report contract remains in platform
and sanitizer jobs; its optional original comparison stays labelled
`local;private-reference`. See [controlled dispatch](sgb-vendor-dispatch.md).

The SPC-reply suite adds `gameboy_sgb_score_vendor_reply` (cost 300,
timeout 900 seconds), bringing the public total to 89 full matrices. It reuses
the distinct/repeated-ID fixtures while checking reply-publication bounds,
unchanged ordinary timelines and exact reset/load playback. The short reply
report and extended observer contracts retain platform/sanitizer coverage.
The optional original comparison remains `local;private-reference`.
See [SPC reply timing](sgb-vendor-reply.md).

The controlled handoff suite adds `gameboy_sgb_score_vendor_handoff` (cost
300, timeout 900 seconds), bringing the public total to 90 full matrices.
It checks separate effect-STOP fields against distinct-note pitch setup,
unchanged ordinary timelines and exact reset/load playback. The short
fixture/report contract retains platform/sanitizer coverage; the optional
original comparison remains `local;private-reference`.
See [controlled SOUND fields and pitch setup](sgb-vendor-handoff.md).


The directory-read suite adds `gameboy_sgb_score_vendor_directory` (cost
300, timeout 900 seconds), bringing the public total to 91 full matrices.
It reuses the eight handoff fixtures on both models, checks selected-word
reads before pitch setup and compares ordinary timelines with observation
removed. Observer lifecycle tests cover exact scalar/batched read traces and
reset/save-load PCM/state. The short report contract remains in platform and
sanitizer jobs; the optional original comparison remains
`local;private-reference`. See [selected directory reads](sgb-vendor-directory.md).


The traversal suite adds `gameboy_sgb_score_vendor_traversal` (cost 300,
timeout 900 seconds), bringing the public total to 92 full matrices. It
checks selected root/pattern/first-note consumption after directory lookup,
retains validation reads, and compares sixteen observed timelines against
sixteen ordinary controls across both models and all STOP/state profiles.
The short report and shared-CLI compatibility contracts retain platform and
sanitizer coverage; originals remain `local;private-reference`.
See [authored score traversal](sgb-vendor-traversal.md).


The request-phase suite adds `gameboy_sgb_score_vendor_request_phase` (cost
600, timeout 1800 seconds), bringing the public total to 93 full matrices.
It checks five request schedules, two STOP profiles, two playback states and
both models, with forty observed timelines and forty ordinary controls.
Its short fixture/report contract remains in platform and sanitizer jobs;
the optional original comparison stays `local;private-reference` (timeout
4800 seconds). See [request-frame phase](sgb-vendor-request-phase.md).


The prior-tempo suite adds `gameboy_sgb_score_vendor_prior_tempo` (cost 600,
timeout 1800 seconds), bringing the public total to 94 full matrices. It
checks two first-track tempos and two request schedules while keeping the
next songs fixed, across STOP/state profiles and both models. It compares
32 observed timelines against 32 ordinary controls and adds four tempo-96
reset/save-load PCM/state replays. Both probe targets are discovered. Short
fixture/report contracts remain in platform and sanitizer jobs; the optional
original comparison stays `local;private-reference` (timeout 4800 seconds).
See [preceding-song tempo](sgb-vendor-prior-tempo.md).

The measured-volume suite adds `gameboy_sgb_score_vendor_volume` (cost 600,
timeout 1800 seconds), bringing discovery to 95 full public matrices. It gates
exact measured voice pairs, held-out setup, provisional fallback, observer
transparency and scalar/native PCM with reset/save-load on both models. The
short `gameboy_sgb_vendor_volume_contract` stays in platform/sanitizer jobs;
the optional original comparison stays private. See
[measured voice volumes](sgb-vendor-volume.md).

The opt-in selection-wait suite adds `gameboy_sgb_score_vendor_selection_wait`
(cost 900, timeout 1800 seconds), bringing discovery to 96 public matrices. It
builds the playback, timeline and direct mid-wait reload probes. Public controls
cover admitted/unlisted tempos, active/completed playback, scalar/native PCM,
wait snapshots and interruption ownership. The short builder/error contract
remains in platform/sanitizer jobs; original timing comparisons remain private.
See [selection pacing](sgb-vendor-selection-wait.md).

The [tempo-112 duration/phase follow-up](sgb-vendor-selection-phase.md) extends
that matrix with four owned completed controls and adds short report/fixture
contracts. Its 30-capture original experiment stays `local;private-reference`
as `gameboy_sgb_vendor_selection_duration_local_reference` (timeout 2400).
Public matrix discovery remains at 96.

The same matrix now includes independent first-request/subframe arrival controls
and their native/scalar reset/save-load replay. The 20-capture arrival experiment
and eight-capture holdout are optional `local;private-reference` tests named
`gameboy_sgb_vendor_selection_arrival_local_reference` and
`gameboy_sgb_vendor_selection_arrival_holdout_local_reference`; each has timeout
2400. They require original program images and stay outside public shards.
Both now enable bounded [timer-access timing](sgb-vendor-selection-timers.md).
Seven short selection-wait contracts remain in platform/sanitizer jobs; four
owned timer-on/off comparisons extend the existing full matrix. Public matrix
discovery remains at 96.
The arrival gates also support `--timer-results`, now used by both optional
CTest entries, to report zero/nonzero hardware timer reads. The ordinary bus,
APU and host observer contracts verify read-clear and callback reset/load
bindings; the same four owned comparisons include the result observer mode.

The matched-duration timer-tempo extension adds 24 owned controls to that same
matrix and two short methods (nine selection-wait contract methods at that step).
The optional `gameboy_sgb_vendor_timer_tempo_local_reference` and
`gameboy_sgb_vendor_timer_tempo_holdout_local_reference` each execute 24 original
captures and retain `local;private-reference` labels with timeout 2400 seconds.
They use equal score durations and different active/completed request spacing;
public matrix discovery remains at 96. See the
[matched timer-tempo results](sgb-vendor-selection-timers.md#matched-duration-tempo-and-state-controls).

Both timer-tempo gates now use `--continuity` to include bounded timer aggregates
through playback and natural completion. The same 24 owned controls validate
their ownership and completion markers, and timer transparency compares four
observation modes. A tenth short contract covers the aggregate bounds, positive
read subsets, completion ownership and shared request timestamps. Public matrix
count remains 96; these optional original gates retain `local;private-reference`.

The offline [persistent accumulator experiment](sgb-vendor-selection-timers.md#persistent-accumulator-prototype)
adds two short methods (twelve total), covering arithmetic, frozen baseline-only
parameter fitting, held-out rejection, corpus identity and bounded sanitized
input failures. It reads existing diagnostic reports and adds no native captures,
private input requirements or public matrices. Successful analysis can report
`accuracy_passed: false`; its exit status does not qualify a firmware candidate.

The [repeated-note tick experiment](sgb-vendor-music-ticks.md) adds 24 owned
controls to the selection-wait matrix and two short methods (fourteen total).
The optional `gameboy_sgb_vendor_tick_local_reference` covers both start schedules,
models and six tempos in 24 original captures, with timeout 2400 and labels
`local;private-reference`. It stays outside public shards. Public matrix count
remains 96 and the existing matrix timeout remains 1800 seconds.

The [tick-phase reselection experiment](sgb-vendor-tick-selection.md) adds 48
owned controls to that same matrix and two short methods (sixteen total).
`gameboy_sgb_vendor_tick_selection_local_reference` optionally captures 48
original timelines across both models, tempos, playback states and start
schedules, retaining timeout 2400 and `local;private-reference`. It remains
outside public shards; public matrix count stays 96.
