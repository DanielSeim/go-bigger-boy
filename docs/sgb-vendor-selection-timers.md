# Timer-read timing around selected score traversal

The [arrival-phase controls](sgb-vendor-selection-phase.md) reject constant
tempo-112 waits and a precise shared 2-ms root cadence. This experiment locates
public timer-register accesses inside the observed interval. It changes no
firmware, timer implementation, production selection or accuracy bound.

The timeline probe's opt-in `--selection-timers` mode includes the existing
directory/traversal observations and aggregates address-only reads at `$FD`,
`$FE` and `$FF`. These are the three public timer-output registers. It records
counts, first/last timestamps and minimum/maximum consecutive-read gaps,
separately from the last selected-directory read to first selected-root read,
and from that root to first KON. No timer values are read or exported by the
observer; it uses the existing SPC read callback without an extra bus access.
Absence of reads does not establish that a hardware timer is disabled.

Each selected-directory byte read resets the window, so owned prevalidation
passes cannot contaminate the final live interval. Unrelated authored directory
slots do not reset it. Timer aggregation stops at the request's first KON.
Each stage/register counter is limited to one million accesses; overflow fails
without a report. Only IDs 1..3 and the fixed authored directory/root addresses
define these windows. Reports are diagnostic metadata, not private RAM/code,
timer values, score assets or PCM.

## Original observations

Eight captures repeat first offsets 0/2 and spin counts 0/1536. Eight independent
captures repeat first offsets 1/3 with the same spins. Both models use duration
48, preceding tempo 112 and later spacing 102 frames. All 16 captures pass the
existing completed, distinct-note, instrument and traversal guards. Every
previously reported phase timestamp, root wait and fixture hash matches the
same schedule's earlier capture without timer collection.

All directory-to-root timer reads use `$FD`, timer 0. No `$FE`/`$FF` reads occur
in that interval, and no timer-output reads occur between root and first KON.
For selection 2, the first `$FD` read is always 0.547852 ms after the last
directory read. The last is normally 0.133789 ms before the root; the held-out
SGB1 offset-3/spin-1536 case has a 0.539063-ms tail. Counts vary with the wait:

| Second-selection root wait | Timer-0 reads |
| --- | --- |
| 15.599609 ms | 1077 |
| 15.998047 ms | 1128 |
| 16.005859 ms | 1129 |
| 17.598633 ms | 1232 |
| 18.003906 ms | 1232 |
| 19.597656 ms | 1387 |
| 20.002930 ms | 1387 |
| 20.409180 ms | 1439 |

The shortest consecutive-read gap is 0.007813 ms. Most maximum gaps are
1.385742 ms; three cases reach 1.791016 ms. Equal read counts can accompany
different wait lengths. Access counts are therefore not elapsed timer ticks.
These observations place repeated timer-0 polling inside the variable stage;
they do not establish the consumed tick count, timer configuration, reset
rules or a phase predictor. Dispatch before the directory read cannot alone
account for the measured difference within this stage.

## Reproduction and public checks

Add `--timers` to either phase-checker command in the arrival-phase document.
For the focused eight-case set:

```sh
python3 scripts/check_sgb_vendor_selection_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --duration 48 --request-offset 2 --first-request-offset 0 --first-request-offset 2 \
  --selection-spin 0 --selection-spin 1536 --timers
```

Each capture retains the 260-million-clock, 240-second and 64-KiB bounds.
The Python summary requires exact register/window fields, ordered ownership,
directory/root agreement with the independent traversal summary, valid stage
timestamps and mathematically consistent counts/gaps. Missing counters,
unknown fields, invalid types and out-of-window reads fail with suppressed
child diagnostics and no partial report. Timers remain opt-in; ordinary reports
retain their previous fields and timing.

The existing optional arrival/holdout CTests now request timer collection and
retain `local;private-reference` labels. The public selection-wait matrix adds
four owned timer-on/off comparisons. Complete non-timer timeline metadata is
identical; excluded tempo 112 performs no timer wait, and the following admitted
tempo-45 selection reads timer 1 in the independent candidate. All four pairs
pass. Seven short fixture/report contracts and the observer lifecycle contract
pass, including exact PCM/state, scalar/batched read sequences, reset and
destination-load callback bindings. Reported original counter gaps also pass
the stricter count/gap consistency checks.

Python compilation and whitespace checks pass. No original PCM comparison,
hardware, independent emulator, Windows/sanitizer run or complete broad matrix
rerun is claimed. Default and opt-in firmware hashes remain unchanged.

The zero/nonzero follow-up below adds the next observation, retaining the
tempo-112 exclusion and all timing bounds.

## Zero versus nonzero hardware reads

`--selection-timer-results` adds a dedicated hardware callback and retains the
address-only observations. The bus performs its ordinary output read and clear
once, then reports timer index 0..2 and whether that returned value was nonzero.
The CPU still receives the exact hardware output. The observer receives neither
that exact value nor any RAM/code values and performs no extra bus read.
The APU wrapper adds the accepted access's physical half-clock timestamp.

Bindings are diagnostic state: bus reset detaches them; APU/host reset preserves
the destination binding. APU and host save/load retain destination callbacks,
including after a rejected load, without serializing pointers or changing the
state format. The engine destructor detaches its physical bus binding.

Each request/stage/register keeps at most 128 nonzero timestamps. Overflow
fails the probe; there is no truncated successful report. Zero counts are the
total accepted reads minus the number of nonzero timestamps. The summary
requires strictly ordered integer timestamps within the existing access bounds,
the result marker and exact fields. Ordinary address-only mode stays unchanged.

All 16 focused original captures pass, including the eight held-out schedules.
Removing only the result fields reproduces the complete earlier address-only
reports exactly, including hashes, counts, gaps and selection timing. Selection
2 contains 8, 9 or 10 nonzero timer-0 reads among 1077–1439 total reads:

| Second-selection wait | Nonzero reads |
| --- | --- |
| 15.599609, 15.998047, 16.005859 ms | 8 |
| 17.598633, 18.003906 ms | 9 |
| 19.597656, 20.002930, 20.409180 ms | 10 |

Consecutive nonzero reads are approximately 2 ms apart (observed range
1.994141–2.006836 ms). The first positive read occurs 1.461914–1.875977 ms
after the directory. This distinguishes the polling cadence from the thousands
of mostly zero reads. It does not prove the exact number of consumed hardware
ticks: a nonzero value may represent multiple accumulated increments, and
four-bit output can wrap. The observer deliberately does not expose that value.

The previously rejected held-out SGB1 root cadence now has a visible local
explanation: offset 3/spin 1536 still has ten positive reads, but root traversal
starts 0.539063 ms after the last, versus the usual 0.133789 ms. The extra
0.405273 ms equals its earlier cadence residual. A fixed number of positive
reads or a fixed post-read tail cannot cover these controls. The remaining
predictor must account for both the differing positive-read count and this tail.

Use `--timer-results` instead of `--timers` in the checker command above;
each result capture retains the 260-million-clock and 240-second limits and
uses a 96-KiB output bound. Missing requested result data fails with suppressed
child diagnostics and no partial report. Both optional arrival CTests now use
this mode; public matrix discovery remains at 96.

The four owned three-mode comparisons pass: plain traversal, address-only
timer timing and timer results have identical shared timeline metadata. The
admitted next selection has positive timer-1 reads; the excluded tempo-112
selection remains unpaced. Seven short report/fixture methods pass, including
invalid/duplicate/out-of-window result timestamps, result counts and refusal
to accept a missing result marker.

Bus tests preserve exact CPU return values and single read-clear behavior on
all three timers, while excluding other IO/RAM inspection and checking detach.
An authored APU loop exercises every timer with audible synthesis and unread
audio at a partial-clock snapshot. APU and host tests preserve exact PCM/state,
callback sequences, reset, destination load and failed-load bindings; host
scalar/batched sequences also match. The authored loop explicitly retains its
IPL overlay and requires sustained timer reads. All four focused CTests pass;
Python compilation and whitespace checks pass. Broad firmware matrices,
Windows/sanitizers, physical hardware and independent emulators were not rerun.
Default and opt-in firmware hashes remain unchanged.

The matched-duration follow-up below compares the remaining tempos and states
before selecting a persistent tempo/phase model.

## Matched-duration tempo and state controls

The new checker covers preceding tempos 32, 45, 64, 96, 112 and 128 on both
models. All three songs have duration 96 in both playback states, so each
active/completed pair has byte-identical score and asset payloads. Active
requests retain 16-frame spacing; completed requests use 124 frames so the
slow tempo-32 note has actually completed. Existing key-off guards verify the
state before both later requests. Earlier controls used duration 48 in completed
cases; those remain unchanged and are not substituted for these matched pairs.

The baseline uses zero first/later offsets and no subframe spin. The held-out
set shifts the first request by two frames, adds two frames to later spacing,
and adds spin 1536 before SOUND 2. Each has 24 captures. All 48 pass ownership,
instrument, playback-state, traversal and timer-result bounds. Only timer 0 is
read between directory and root; no timer-output reads occur from root to KON.

| Preceding tempo | Active nonzero reads | Completed nonzero reads | Root-wait range across both sets |
| --- | --- | --- | --- |
| 32 | 16 | 16 | 31.595703–32.000977 ms |
| 45 | 13 or 14 | 13 or 14 | 25.549805–28.456055 ms |
| 64 | 12 | 12 | 23.553711–24.008789 ms |
| 96 | 9 or 10 | 10 | 18.001953–20.409180 ms |
| 112 | 9 or 10 | 8 or 10 | 15.998047–20.409180 ms |
| 128 | 10 | 10 | 19.597656–20.454102 ms |

Consecutive positive reads remain 1.995117–2.006836 ms apart. The post-positive
tail spans 0.130859–0.584961 ms, including active controls: the longer tail is
not specific to natural completion. At held-out tempo 45, SGB1 active playback
has 14 positive reads and a 0.584961-ms tail, while SGB2 has 13 and 0.539063 ms.
At held-out tempo 112, completed SGB1 has ten positive reads and a 20.409180-ms
wait, while completed SGB2 has eight and a 15.998047-ms wait. Duration is equal
in these pairs. Model, tempo and active/completed state alone still do not
provide a general wait predictor; a fixed positive-read count also fails.

The report decomposes selection 2 into first/last positive-read times, minimum
and maximum positive-read gaps, zero/nonzero counts and time after the final
positive read. It retains all bounded per-selection timer observations and
actual request/directory/root timestamps. Zero-positive-read cases remain
reportable with null decomposition times rather than being mistaken for ticks.
Nonzero reads are still not exact accumulated hardware tick counts.

```sh
python3 scripts/check_sgb_vendor_timer_tempo_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
python3 scripts/check_sgb_vendor_timer_tempo_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --request-offset 2 --first-request-offset 2 --selection-spin 1536
```

Tempo/state filters repeat and deduplicate; all schedule parameters have bounded
CLI choices. Each immutable fixture has a unique model/tempo/state filename,
with at most four captures concurrent. The longer matched schedule uses a
300-million-clock budget, retaining 240 seconds and 96 KiB per child. The
initial 260-million-clock matched attempt was rejected before a complete third
selection; it produced no report and is not behavior evidence. Failed captures
or missing result markers suppress child diagnostics and produce no partial
JSON. Successful reports retain `qualification: false`. Their per-group
constant-delay feasibility ranges are diagnostic, not a candidate timing gate.

The builder's optional `completed_frames=124` is used only by these controls;
the default remains 100 and existing images are unchanged. The shared phase
summary now accepts either playback state, retaining its authoritative guards;
active reports have null prior-release age. Short methods check payload equality,
invalid controls, decomposition, deduplication, state propagation and sanitized
failure. All nine short methods pass. The 24 owned matched controls also pass
on both models/states, retaining the admitted waits, unlisted tempo-112 fallback,
next-selection tempo-45 wait and positive timer-1 observations in the candidate.

Both original gates remain optional `local;private-reference`. The public
controls extend the existing matrix, retaining all earlier cases and the total
of 96 matrices. Python compilation, CMake configuration and whitespace checks
pass. No new broad-matrix, hardware, independent-emulator or platform/sanitizer
validation is claimed. No original instructions, private score/sample bytes,
RAM snapshots or PCM are used as source or committed. Default and opt-in
firmware hashes and the existing timing accuracy bound remain unchanged.

Next, track timer phase through the preceding song and natural completion.
Use that state to prototype a persistent accumulator and compare it against
these frozen schedules, keeping the current waits until the candidate passes
the direct timing and lifecycle gates. Tempo 112 remains excluded.

## Timer phase through natural completion

The timeline probe's opt-in `--timer-continuity` retains the existing selection
observations and adds bounded post-KON timer aggregates. For each request it
records all-read and positive-read counts, first/last timestamps and minimum/
maximum gaps for FD, FE and FF, split at the first KOF-255 write after KON.
That marker denotes score completion in these independently authored single-note
fixtures; it does not assert that every DSP envelope has become silent. The
window ends when the next packet arrives, or when the bounded capture ends.
No timer output values, PCs, private RAM, score/sample bytes or PCM are exported.

Each stage/register accepts at most one million reads. Storage stays fixed
rather than retaining every positive timestamp during long playback. The
Python validator checks field sets, strict integer types, count/gap consistency,
positive-read subsets and the request/KON/completion anchors. A read immediately
before packet delivery can share its recorded timestamp, so stage endpoints
allow equality while ownership follows the delivered-packet count in the
callback. Reads beyond that endpoint remain invalid. This boundary was caught
by the first full run; the failed report was discarded and the bounded saved
baseline captures were revalidated after adding a regression test.

The matched-duration checker accepts `--continuity`, keeping duration 96,
completed spacing 124 frames, the 300-million-clock limit, four concurrent
captures, 240-second child timeout and 96-KiB output bound:

```sh
python3 scripts/check_sgb_vendor_timer_tempo_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --continuity
python3 scripts/check_sgb_vendor_timer_tempo_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms \
  --continuity --request-offset 2 --first-request-offset 2 --selection-spin 1536
```

Baseline evidence covers both models, all six preceding tempos and both states
(24 original captures). Every completed first song continues reading timer 0:
185–812 positive reads occur after its completion marker and before the next
request. Active first songs have no completion marker or idle reads. Playback
positive-read maximum gaps are 2.006836 ms; completed idle maximum gaps are
2.004883 ms. These are observed positive reads, not exact accumulated tick counts.

The last positive FD read before reselection and the first after the selected
directory are separated by 10.003906–63.996094 ms. Their distance to the nearest
multiple of 2 ms is -0.004883 to +0.006836 ms. This is evidence of persistent
hardware timer phase across the observed transition, including completed songs.
The intervening dispatch reads are not retained by these aggregates, so the
bridge is not a claim that every intervening tick was read once. All fields
shared with the earlier matched baseline report remain identical.

The held-out schedule adds another 24 original captures. Its completed first
songs have 210–837 positive idle reads, with gaps of 1.994141–2.004883 ms.
Across the completion boundary itself, the last playback positive and first
idle positive are 1.997070–2.005859 ms apart over both schedules. Held-out
reselection bridges span 23.998047–65.994141 ms, with residuals from the nearest
2-ms multiple of -0.006836 to +0.005859 ms. All earlier held-out report fields
also remain identical. Playback aggregates contain positive-read gaps as short
as 0.795898 ms; they do not locate those gaps, so these results do not establish
universal 2-ms spacing for every positive read during playback.

All ten short selection-wait methods pass, including missing-continuity failure
sanitization. The 24 owned matched controls pass with the new flag on both
models and states. Four additional owned model/schedule cases compare plain
traversal, timer addresses, timer results and continuity; removing the added
fields leaves identical existing playback metadata in each case. The existing
observer lifecycle contract also passes. The native probe rebuild, Python
compilation, CMake configuration and whitespace checks pass. Full unrelated
matrices, platform/sanitizer jobs, hardware and independent-emulator validation
were not rerun. No original PCM comparison is claimed.

This observation supports retaining timer phase after natural completion. It
does not yet explain the variable number of positive reads consumed before
score traversal, or establish a general software tempo accumulator. The five
admitted waits, tempo-112 exclusion, default firmware and opt-in image remain
unchanged. Next, prototype an independent persistent tempo accumulator against
these frozen baseline and held-out schedules, requiring the direct accuracy,
improvement and lifecycle gates before expanding the admitted tempo set.

## Persistent accumulator prototype

`scripts/check_sgb_vendor_accumulator_model.py` implements an independently
written diagnostic hypothesis: a modulo-256 phase advances by the authored
preceding tempo on each inferred 2-ms hardware opportunity. It persists through
natural completion. Reselection waits for the next accumulator carry plus a
single global number of guard opportunities. A fixed 274-half-clock
(0.133789-ms) handling allowance represents the previously observed short tail.
The hardware phase anchor is the latest positive FD read before the next
request; the initial software phase is defined at the first song's KON.

This uses public timestamps, authored tempo and fixture identity. It does not
use timer output magnitudes, positive-read counts as tick increments, original
instructions or private memory. Inferring regular opportunities from the last
hardware anchor is an explicit hypothesis, not proof that all original reads
or ticks follow this schedule. The model does not reproduce host dispatch or
claim that a candidate would reach the same selected-directory timestamp.

The finite fit enumerates initial phases 0–255 and guard counts 0–16. It chooses
one global pair across the 24 baseline cases, minimizing maximum absolute error,
then total absolute error, then phase/guard for deterministic ties. No per-model,
per-state or per-tempo parameters are fitted. All arithmetic and comparisons
use integer APU half clocks. The held-out schedule cannot influence parameters.

The input loader requires the complete 24-case matrices, the fixed schedules
(0,0,0) and (2,2,1536), matched duration/spacing, identical original program hashes,
independently rebuilt fixture hashes and consistent public ownership/timing
anchors. Each report is bounded to 1 MiB. Malformed input produces no partial
report and suppresses input diagnostics. This is an offline analysis of the
previously validated reports; no new original execution is needed:

```sh
python3 scripts/check_sgb_vendor_accumulator_model.py \
  --baseline /tmp/gbb-selection-wait/continuity-baseline.json \
  --holdout /tmp/gbb-selection-wait/continuity-holdout.json
```

The final integer fit selects phase 69 and eight guard opportunities. It fails
the existing 2-ms bound:

| Schedule | Cases within 2 ms | Maximum absolute error |
| --- | --- | --- |
| Baseline used for fitting | 18 / 24 | 4.004883 ms |
| Held out after fitting | 17 / 24 | 10.447266 ms |

The largest held-out miss is active SGB1 tempo 45: 18.008789 ms predicted versus
28.456055 ms observed. Completed SGB1 tempo 45 misses by 8 ms. Even the fitted
baseline fails, so this particular one-carry-plus-guard model is rejected.
Changing or resetting its initial phase is not a supported firmware fix. The
report retains `qualification: false` and `accuracy_passed: false`. Exit zero
means the analysis completed, not that the model passed a candidate gate.

Twelve short contracts pass. New checks cover split versus continuous phase
advancement, carry arithmetic, invalid inputs, exact synthetic fits, unchanged
parameters when held-out outcomes change, rejection of held-out inaccuracies,
fixture/schedule identity, duplicate cases and sanitized oversized/malformed
reports. Python compilation, CMake configuration and whitespace checks pass.
This step reruns no native audio, lifecycle or unrelated full matrices because
it changes only the offline model, tests and documentation. Default and opt-in
firmware hashes and the tempo-112 exclusion remain unchanged.

Next, author bounded repeated short-note fixtures that expose software music-tick
intervals through public KON/KOF writes, alongside timer phase. That can test
whether music processing advances at the predicted accumulator carries before
another scheduler is tried. Compare both models and independent arrival
schedules with the existing event bounds; use authored score/assets and public
register/timestamp observations. A failed simple hypothesis does not establish
that all persistent-accumulator approaches are impossible.
