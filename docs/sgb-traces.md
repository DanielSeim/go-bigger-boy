# SGB trace and replay

The core exposes a small deterministic trace format for investigating Super
Game Boy behavior without bundling a Nintendo BIOS or ROM data. A trace records
the JOYP writes presented to the clean-room SGB adapter, their bus-cycle
positions, decoded command packets, and checkpoints containing the SGB
framebuffer hash, complete diagnostic state hash, and adapter counters.

`gameboy::SgbTrace::Recorder` is used by tests and diagnostic frontends:

1. Construct it with the source emulator's ROM fingerprint and hardware model.
2. Record each value written to `FF00` with its bus-cycle position.
3. Add checkpoints after packet completion or after a transfer has advanced.
4. Serialize the trace and replay it against the same user-supplied ROM.

The headless runner exposes the same workflow without writing a frontend:

```sh
gbb_test_runner path/to/sgb.gb --model sgb --max-cycles 5000000 \
  --sgb-trace /tmp/sgb.trace
gbb_test_runner path/to/sgb.gb --model sgb \
  --replay-sgb-trace /tmp/sgb.trace
```

Capture writes the trace when the run passes, fails, or reaches its cycle
limit. It is deliberately opt-in and enables the existing bounded I/O trace
buffer only while capture is active.

On Android, enable **Capture SGB diagnostic trace** in Settings → Data and
saves, then start the SGB/SGB2 ROM again. While the game is running, or after
it exits, use **Export latest SGB trace** to save the text trace through the
Android file picker. The recorder is off by default, only attaches to SGB and
SGB2 hardware, and stores the trace in memory until it is exported; it does
not upload ROM data or alter ordinary non-SGB runs.

To compare two captures from different builds or devices, use the standalone
diff tool:

```sh
gbb_sgb_trace_diff desktop.trace android.trace
```

An exit status of `0` means the traces match. The comparator ignores the
headless runner's synthetic startup checkpoint (`cycle=0`, `frame=0`) and its
synthetic final boundary checkpoint (`frame=0` after a positive frame count),
so captures from the desktop runner and Android lifecycle can be compared
directly. All JOYP writes and real frame checkpoints remain strict. Exit status
`1` identifies the first differing write, checkpoint, diagnostic counter, or
decoded command; exit status `2` indicates that one of the trace files could
not be parsed.

Replay advances the bus to each recorded cycle, reapplies the JOYP writes, and
reports the first framebuffer, state, diagnostic, or decoded-command mismatch.
The parser and replay path have bounded write, checkpoint, line, and file sizes;
malformed traces are rejected before they can allocate unbounded storage.

The trace stores a ROM fingerprint, not the ROM itself. This keeps diagnostic
artifacts suitable for sharing while requiring the investigator to provide a
legally obtained copy of the matching cartridge. The format is intentionally
text-based so a failing packet sequence can be inspected in a code review.
To count the decoded commands across one or more opt-in captures, run
`python3 scripts/report_sgb_commands.py capture-a.trace capture-b.trace`.
The inventory labels SNES-side commands that the current HLE host cannot
execute; see [SGB validation](sgb-validation.md) for the triage policy.
For a locally supplied title, the SHA-pinned manifest workflow described
there also captures the full SGB frame and marks whether an independent
visual reference was actually compared. Command recognition alone is not
visual or audio validation.
