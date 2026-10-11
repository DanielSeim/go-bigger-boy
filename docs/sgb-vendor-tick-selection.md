# Measured music phase before reselection

The [repeated-note experiment](sgb-vendor-music-ticks.md) separates first-note
startup from subsequent music ticks. This extension supplies phase witnesses
before a long note and a second song request, allowing active/completed
reselection to be compared with the measured music phase.

`build_sgb_vendor_tick_selection_fixture.py` authors four duration-2 notes
(A4/A3/A4/A3), then a duration-96 A2 tail. The next selection is a duration-96
A3 at tempo 45. Both songs use the existing independently authored instrument-2
binding and looping BRR fixture. Six preceding tempos (32, 45, 64, 96, 112, 128)
are tested. Active requests use a 16-frame gap; completed requests use 124.
The other schedule offsets the first request and gap by two frames and applies
the existing 1536-spin control before the second request. Score/assets are
identical between schedules and playback states.

Every capture must reach the tail before reselection. Completion state comes
from the first request's KOF-255 marker before the next request, not its frame
spacing. The second song must complete within the capture. The summary checks
all six pitch/binding/ownership tuples, prefix gates and clears, per-request
first-KON continuity and complete live directory/root/pattern/track traversal.
Existing timer-window validation checks the selected-root timing and bounded
zero/nonzero read metadata. No emulator or firmware behavior changes.

The checker executes originals opaquely and retains only public DSP writes,
authored-address timestamps and bounded hardware timer-read metadata. It
exports no original instructions, PCs, private score/sample bytes, RAM snapshots,
exact timer values or PCM. Each capture uses 300 million SNES clocks, a
240-second timeout and 96-KiB child output bound. At most four immutable fixture
cases run concurrently. Failures suppress child diagnostics and emit no partial
JSON. Reports retain `qualification: false`.

```sh
python3 scripts/check_sgb_vendor_tick_selection_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The music-phase fit uses three onset intervals after the second KON, including
the transition to the long tail. It minimizes maximum error, then total error,
then phase for deterministic ties. Only those prefix timestamps select phase;
no request, root-wait or completion outcome tunes it. Separately, all phases
consistent with those prefix intervals within 1 ms are retained as uncertainty.
This tighter witness tolerance does not change the 2-ms root-wait accuracy bound.
These fitted phases are model parameters, not observations of original RAM.

The previously independent one-carry-plus-guard hypothesis is then tested with
the measured second-KON phase instead of a globally guessed phase. Inferred
2-ms opportunities advance through the tail and idle; completion does not reset
phase. The latest positive timer-0 timestamp anchors hardware phase. The fixed
274-half-clock handling allowance is unchanged. One guard count, 0–16, is fitted
across the 24 baseline root waits and frozen for the other 24 cases. The prefix
phase is an observable witness per case, rather than a root-outcome fit.

A separate optimistic feasibility check gives each baseline case every phase
supported by its prefix, and tests every guard count. This bounds the model's
best possible timing under those assumptions; it is not a runtime predictor.
Compatible prediction ranges in the report likewise describe uncertainty and
must not be used to choose a phase after observing the root outcome. The final
range analysis was computed from the captured prefix timestamps, without
additional original execution.

All 48 original captures complete with valid state/traversal guards. Prefix
maximum error is 0.494141 ms; cases admit 6–128 initial phases within 1 ms.
Measured selected-directory-to-root intervals are:

| Preceding tempo | Minimum | Maximum |
| --- | --- | --- |
| 32 | 31.595703 ms | 32.000977 ms |
| 45 | 25.549805 ms | 28.456055 ms |
| 64 | 23.545898 ms | 24.005859 ms |
| 96 | 18.001953 ms | 20.409180 ms |
| 112 | 16.005859 ms | 20.409180 ms |
| 128 | 19.597656 ms | 20.454102 ms |

The point fit selects eleven guard opportunities. None of its 24 baseline or
24 other-schedule predictions meets the 2-ms bound. Maximum absolute errors are
9.997070 ms and 11.998047 ms respectively. More strongly, no tested guard count
can satisfy all baseline cases even with per-case freedom over every compatible
prefix phase. The smallest optimistic maximum baseline error is 9.997070 ms.
The completed other-schedule SGB2 tempo-112 case predicts 28.003906 ms versus
16.005859 ms observed; its entire compatible phase set gives that prediction.

This rejects the tested continuous old-tempo carry plus fixed guard model under
its stated clock assumptions. Better music-phase initialization alone does not
repair it. It does not prove that all accumulator schedulers are impossible,
or establish whether selection resets hardware timers or software state. The
current five admitted waits, excluded tempo 112, default image and opt-in image
remain unchanged. No replacement candidate is qualified by this report.

All sixteen short contracts pass, covering payload identity, state/ownership/
binding/gate/traversal rejection, prefix fits, prediction uncertainty, conflicting
guard constraints, deduplication and sanitized failure. Forty-eight owned
controls pass their prefix accuracy, active/completed classification, admitted
waits and tempo-112 fallback on both models/schedules. CMake configuration,
Python compilation and whitespace checks pass. Unrelated full matrices,
platform/sanitizer, lifecycle, PCM, hardware and independent-emulator checks
were not rerun because no core or firmware changed.

Next, observe bounded writes to the public timer control and target registers
around selection, retaining observer transparency and reset/save/load contracts.
That can distinguish a timer restart or rate change from a software scheduling
transition before another replacement scheduler is attempted.
