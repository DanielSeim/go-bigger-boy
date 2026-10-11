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

Next, measure these positive-read counts across the admitted preceding tempos
and active/completed states before selecting a persistent tempo/phase model.
Use those observations to test a candidate against reserved schedules rather
than replacing the rejected fixed delay with another fitted constant.
