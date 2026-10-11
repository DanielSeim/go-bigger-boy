# Timer configuration through reselection

The [measured phase experiment](sgb-vendor-tick-selection.md) rejected the
persistent accumulator-plus-guard model even when using the preceding notes as
phase witnesses. This follow-up checks whether hardware timer configuration
changes during selection, playback or natural completion.

The bus observer reports accepted writes to F1 masked to its three timer-enable
bits, and FA/FB/FC target writes. It runs after normal write effects. Repeated
writes remain observable: a disable/enable sequence can restart a timer even
without changing its target. Other F1 control bits, RAM, DSP RAM accesses and
exact timer-read values are excluded. APU callbacks add public half-clock time;
the host retains destination callback bindings through reset and state restore.
Pointers are not serialized and the save-state format is unchanged.

The opt-in timeline flag `--timer-writes` includes existing continuity, timer-read
and authored-address guards. Before the first sound request, it retains only
four bounded write counts and the latest configuration. Each request snapshots
that configuration before its first write. Unknown targets remain -1 until
written; that sentinel does not claim a reset value. At most 16 request snapshots
and 512 subsequent writes are retained, with overflow rejecting the capture.

The checker validates exact fields, address/value masks, monotonic timestamps,
request ownership and replay from each initial snapshot to the final state.
It summarizes dispatch, directory-to-root wait, setup, playback and post-completion
idle separately, including enable rising edges, known target changes and initial
writes to previously unknown targets. Existing note, traversal, state and phase
checks remain mandatory.

```sh
python3 scripts/check_sgb_vendor_timer_writes_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The corpus reuses the authored four-note prefix, long tail and second song at
six preceding tempos (32, 45, 64, 96, 112, 128), both SGB models, active/completed
states and both schedules: 48 captures. Each uses 300 million SNES clocks,
a 240-second timeout and a 96-KiB output limit, with four parallel cases.
Failures suppress child diagnostics and partial JSON. Reports explicitly retain
`qualification: false`. Originals run opaquely; no original instructions, PCs,
score/sample bytes, RAM snapshots or PCM are exported.

All 48 local original captures passed the existing phase/state guards. Every
request began with configuration `[1,16,-1,-1]`: timer 0 enabled, target 16,
a 2-ms period; timer 1/2 targets were never written. SGB1 boot write counts were
`[9,3,0,0]`, SGB2 counts `[7,2,0,0]`. There were **zero configuration writes**
from the first sound request through the second song's completion and capture
end. Every shared timing/phase field matched the previous 48-case capture
exactly. The added zero-valued unknown-target initialization counters were
aggregated from these captured empty write lists without further execution.

These results rule out timer restart or retargeting as the cause of selection
delay in this corpus. They support investigating software tempo state; they do
not identify its algorithm or qualify audio equivalence. Private captures remain
local and are not public CI inputs.

Public bus, APU and host tests compare exact PCM and saved state with observation
on/off, scalar/batched traces, reset, successful destination load, failed load
and detach. Owned controls exercise actual timer writes across the same 48-case
matrix; four additional paired captures compare all existing timeline metadata
exactly with observation enabled/disabled. Short contracts reject malformed
configurations and unsafe partial diagnostics.

Default and opt-in firmware images remain unchanged. Program replacement is
unfinished and external overrides remain available. The next experiment should
vary the authored tempo before the tail independently from the initial tempo,
then compare selection delay against both. That can distinguish retained
initial tempo from live playback tempo before changing the firmware model.
