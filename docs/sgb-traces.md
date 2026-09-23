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

Replay advances the bus to each recorded cycle, reapplies the JOYP writes, and
reports the first framebuffer, state, diagnostic, or decoded-command mismatch.
The parser and replay path have bounded write, checkpoint, line, and file sizes;
malformed traces are rejected before they can allocate unbounded storage.

The trace stores a ROM fingerprint, not the ROM itself. This keeps diagnostic
artifacts suitable for sharing while requiring the investigator to provide a
legally obtained copy of the matching cartridge. The format is intentionally
text-based so a failing packet sequence can be inspected in a code review.
