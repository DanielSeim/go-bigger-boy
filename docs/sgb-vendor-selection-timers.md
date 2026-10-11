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

Next, observe whether timer-0 reads consume zero or nonzero hardware output,
without exporting exact counter values. Validate that dedicated observation's
read-clear behavior and reset/save-load transparency before using consumed-tick
timing to design a phase-aware candidate. Tempo 112 remains excluded.
