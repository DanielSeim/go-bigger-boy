# Starting tempo versus live playback tempo

The [timer-configuration experiment](sgb-vendor-timer-writes.md) found no timer
restart or retargeting during selection in 48 original captures. This follow-up
separates a song's starting tempo from its later playback tempo using authored
E7 commands, without inspecting original software state.

`build_sgb_vendor_tick_selection_fixture.py` accepts optional `live_tempo` /
`--live-tempo`. With that option, the original duration-2 A4 is followed by E7
and the new tempo, then the existing A3/A4/A3 prefix and duration-96 A2 tail.
The second selected song stays at tempo 45 with duration-96 A3. Instrument-2
binding, BRR assets, directories and pattern pointers are unchanged. Omitting
the option preserves all 48 prior fixture hashes exactly.

The corpus crosses starting and live tempos 32 and 128, on both models, both
active/completed playback states and both request schedules: 32 captures.
Unchanged-tempo controls also execute the inserted E7, so changed cases are not
compared against fixtures lacking the command. Assets and score payloads remain
identical between playback states and schedules for each tempo pair.

The first-note interval must fit the starting tempo's possible two-tick
intervals with a fixed 4-ms allowance. The earlier repeated-note experiment
already documented distinct first-note startup variation, including a 10.159-ms
interval at tempo 128. An initial 2-ms allowance rejected that known startup
behavior; the guard uses 4 ms while still separating the two starting tempos.
The three subsequent intervals must fit the live tempo within 2 ms. This
checks that both tempo commands actually took effect before interpreting any
selection timing. Existing note/binding, gate/clear, traversal, timer-continuity
and active/completed ownership checks remain mandatory. Timer configuration is
validated and reported as in the preceding experiment.

Each changed case is compared against two unchanged controls of the same model,
state and schedule: one matching its starting tempo, the other its live tempo.
Reports include control separation, absolute timing differences and the fixed
2-ms comparison flags. No parameters are fitted, and neither schedule's outcomes
are used to tune a predictor. These are matched contrasts, not a general timing
algorithm or firmware qualification.

```sh
python3 scripts/check_sgb_vendor_tempo_change_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

Each capture uses 300 million SNES clocks, a 240-second timeout and a 96-KiB
output limit, with four parallel immutable fixtures. Originals execute opaquely.
Only public DSP writes, authored-address timestamps and bounded timer metadata
are retained; no original instructions, PCs, score/sample bytes, RAM snapshots
or PCM are exported. Failures suppress child diagnostics and partial JSON.
Reports retain `qualification: false` and original/fixture hashes. Private
reports remain local and are not public CI inputs.

All 32 local original captures passed. The 16 changed-tempo contrasts (eight
per schedule) matched their live-tempo controls within 0.451172 ms; none matched
their starting-tempo control within 2 ms. Starting-tempo errors were
11.092773–12.404297 ms, with unchanged-control separation of
11.186523–12.403320 ms. Observed selected-directory-to-root intervals were:

| Live tempo | Minimum wait (ms) | Maximum wait (ms) |
| --- | ---: | ---: |
| 32 | 31.546875 | 32.407227 |
| 128 | 19.597656 | 20.454102 |

Starting-tempo first intervals were 29.827148–30.730469 ms at tempo 32 and
6.278320–10.276367 ms at tempo 128. Maximum later-prefix fit error was
0.585938 ms. Every initial timer configuration was `[1,16,-1,-1]` and no timer
configuration writes occurred during either request. Both baseline and other
schedule give the same qualitative result, in active and completed playback.
This supports retention of the live tempo in this corpus and rejects retention
of only the initial tempo. It does not recover the selection-delay algorithm.
The current opt-in firmware already retains the later tempo; no firmware change
is justified by this result.

Public controls use the unchanged opt-in firmware on the same 32-case corpus.
They require the existing admitted wait for the live tempo and validate both
tempo witnesses. Four additional held-out active cases cover both change
directions and models, comparing native/scalar PCM hashes and timing while the
owned playback probe checks reset, state restoration, unread PCM and snapshots
during the selection wait and its timer pulses. Short contracts also reject
missing/duplicated contrasts, wrong tempo witnesses and malformed captures.

The default and opt-in firmware images remain unchanged, as do external
firmware overrides. This experiment narrows the selection state question; it
does not finish the program replacement or establish complete audio equivalence.

Next, vary how long the new tempo has been active before reselection, retaining
the same starting/live pair and authored playback witnesses. Moving the command
closer to the request can distinguish immediate adoption from state that settles
over subsequent music ticks, before attempting another general timing model.
