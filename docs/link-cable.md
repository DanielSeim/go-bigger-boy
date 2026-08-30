# Link cable milestone

The core now models the Game Boy serial port at bit granularity. `SB` (`FF01`)
is shifted one bit per clock edge and `SC` (`FF02`) controls transfer start,
clock selection, and CGB fast mode. DMG transfers use 512 CPU clocks per bit
(8192 Hz); CGB fast mode uses 16 clocks per bit.

`gameboy::SerialCable` connects two `gameboy::SerialPort` instances without
threads or I/O. The console selecting the internal clock drives both ports,
while the other port receives the same edges as an external-clock device. If
both consoles briefly request an internal clock during Pokémon's connection
handshake, the first request deterministically owns the cable and the losing
side retains its preceding external probe byte, matching the hardware race
that establishes one internal and one external role. A missing endpoint
supplies pull-up `1` bits, matching the disconnected cable state. Transfer
completion raises serial interrupt 3 on both consoles.

Frontends should use `gameboy::LinkSession` when they own two emulator
instances. The session owns the cable, exposes `disconnected`, `starting`,
`connected`, `transferring`, and `timed_out` lifecycle states, and advances
both CPUs with a cycle-balanced scheduler. This keeps link timing policy out
of SDL and makes reconnect behavior testable without a window or network
transport. The SDL split view shows the current state and completed-transfer
count in a small status strip. A transfer that makes no bit or completion
progress for the session watchdog budget enters `timed_out`; retrying the
session resets only serial protocol state and cable arbitration, preserving
both running machines and their saves. On desktop, **Emulation → Retry Link
Handshake** (or `Ctrl+Shift+R`) performs that recovery.

`gameboy::LinkTransport` is the seam for additional transports. The shipped
`LocalLinkTransport` wraps `SerialCable`; transport implementations must queue
network I/O outside the serial edge callback and present only ready edges to
the emulation thread. `LinkPacketCodec` defines the fixed `GB`/versioned,
checksummed frame used by the planned TCP backend.

`gameboy::TcpLinkChannel` provides the first transport-layer implementation:
loopback host/connect, non-blocking polling, partial-write handling, and frame
validation. It enables TCP's low-latency mode for the small serial request and
response frames, avoiding packet coalescing delays while leaving the socket
non-blocking. It is intentionally not selectable from the SDL session yet; a
remote serial-edge adapter must consume queued packets without ever waiting
inside the CPU or serial callback.

`gameboy::TcpSerialEndpoint` is the corresponding remote-edge adapter. It
queues an outgoing bit during `prepare_bit`, holds the local internal clock at
the next edge until a response arrives, and services incoming requests by
clocking the local external port from `poll()`. This keeps socket latency out
of CPU execution; a host UI can now compose one endpoint, one channel, and one
emulator for a remote session. While a TCP bit is in flight, the endpoint also
preserves the partial shift register across Pokémon's repeated SB/SC probe
rewrites; an explicit link reset remains the cancellation boundary.

The printer and test-ROM serial-output paths remain available through the
`MemoryBus` adapter. A serial endpoint is deliberately not embedded in a save
state because it belongs to the host session; hosts should reconnect a cable
after loading. Older save states resume a pending transfer at the next clean
serial edge.

The SDL desktop frontend now exposes that deterministic local path. With a ROM
running, choose **Emulation → Start Local Link Session** (or press
`Ctrl+Shift+L`). GBB starts a second instance of the same ROM, connects both
serial ports, and presents the two 160×144 screens side by side. Player one
uses the configured controls; player two defaults to `W/A/S/D` for directions,
`J/K` for A/B, and `Q/E` for Select/Start. The command toggles back to the
single-console view and reconnects cleanly when a new ROM is loaded. Battery
games receive an independent player-two save under `link-saves/`; it is seeded
from the primary save the first time and then persists separately, so each
console can keep its own trainer identity and party. When the session starts,
player two has its own persistent player-two save, but its running machine is
initialized from the primary console's current state so both screens begin at
the same map or Cable Club prompt. The two consoles then run independently and
their battery images remain separate. For Pokémon Red/Blue/Yellow, the
transient HRAM link marker and all serial scratch registers are cleared on
attach, and the ROM performs its normal internal/external role probe. The
cable deterministically resolves any simultaneous clock request while
preserving the earlier probe byte. The Game Boy Printer is detached from both
serial ports for the duration of the link session and restored afterward. In
the default player-two layout, press `J` to advance player two's prompt. The
cable exchanges
individual bits at emulated
hardware time, including simultaneous internal-clock probes. A connected
internal clock holds its first edge until the peer has armed its serial
receiver; this avoids losing a startup byte when the two emulated CPUs reach
the handshake a few instructions apart, without slowing either CPU.

On desktop, **Emulation → Host TCP Link** (`Ctrl+Shift+H`) listens on
`127.0.0.1:8765`, while **Join TCP Link** (`Ctrl+Shift+J`) connects to that
endpoint. Use one emulator instance in host mode and another in join mode,
with the same ROM and prepared Cable Club saves. For Pokémon Gen I, have the
host player talk to the Cable Club attendant and confirm the link first; the
join player should then confirm on its side. This gives the game's serial
handshake a clock owner before the peer starts its probe. The single-screen
status strip shows the TCP role and state (`W` means the TCP socket is
connected but the link handshake is still waiting for the host); **Retry Link
Handshake** reopens the same endpoint after a disconnect, and **Stop TCP Link**
restores the printer.

In a TCP session the host wins the initial clock race. Clock ownership is then
released explicitly after each completed byte, allowing Pokémon to alternate
which side clocks subsequent exchanges without permitting simultaneous clocks.
An explicit serial/link reset also sends an ordered, sequence-numbered reset
marker, so a request already queued in the peer is discarded before a retry
can arm a new byte without canceling a newer request.

Each remote instance receives its normal primary keyboard bindings (arrows for
movement, `X` for A/confirm, `Z` for B/cancel, and Enter for Start by default).
The `W/A/S/D` and `J/K` player-two bindings apply only to the local split-screen
session. Click a window to focus it before navigating its Cable Club menu.

Link tracing is opt-in. Add `link.Diagnostics = true` to the portable
`settings.ini` beside the executable before starting a session; normal users
therefore get no diagnostic popup and no trace file. When enabled, the trace is
reset for each local session. It begins with a `session_start` marker and ends
with `session_end`; transfer counters therefore describe only that session, and
frame numbers are written in decimal for easier correlation with a reproduction.
Each frame also records CPU cycle totals, PC/SP, halt/stop status, serial phase,
interrupt registers, and (for Pokémon Gen I) the game link/battle markers and
party count. Additional `event=serial_complete` and `event=serial_active` lines
make byte completions and clock ownership changes easy to locate without
manually diffing every frame. The session header identifies the transport and
role, so host and join logs can be compared directly.

Network, WebRTC, Bluetooth, and USB transports should be added only after this
deterministic local path is validated with link-enabled games.

## Headless integration harness

`gbb_link_harness` runs two real emulator cores over the TCP serial endpoint
without writing back to the supplied ROM or save files. It reports handshake
and transfer counters, unmatched responses, emulated time, and before/after
battery-save fingerprints. The optional `--auto-confirm` mode sends periodic
`A` pulses for Cable Club prompts; it is intended as a first-pass smoke test,
not as a replacement for the manual trade/battle checklist.

Keep private ROMs and saves under the ignored `roms/` directory and run:

```sh
./build-sdl/gbb_link_harness \
  --rom roms/pokemon-blue.gb \
  --save1 roms/player1.sav \
  --save2 roms/player2.sav \
  --frames 1200 --auto-confirm \
  --report /tmp/gbb-link-report.txt
```

Battery saves contain cartridge RAM but not the CPU/WRAM position. For a run
that starts directly at the Cable Club, first create one GBB full save state
per player while each game is positioned at the same prompt, then add:

```sh
  --state1 /path/to/player1.gbbs \
  --state2 /path/to/player2.gbbs
```

The two state files must be made from the same ROM and should be captured
before attaching the link. The harness still imports the supplied battery
saves, so save-state loading does not overwrite the originals.

Use `--transport local` to run the same ROM/save pair through the deterministic
in-process cable when the host operating system blocks loopback sockets. Use
`--transport tcp` (the default) on a native desktop to exercise host/join TCP.

The harness can also assert the game-level result, rather than treating serial
traffic alone as success. Add `--expect trade` for a trade run or
`--expect battle` for a battle run. A trade is accepted only when both parties'
snapshots changed and each final party contains a complete Pokémon record that
came from the other party's initial snapshot. A battle is accepted only after
both emulators have entered the link-battle state during the run. The report
always includes the before/after party summaries, change flags, and the
`trade_observed`/`battle_observed` results. With `--expect`, a missing semantic
result is reported and the process exits nonzero even when all TCP transfers
completed; this prevents a reserved-area, timeout, or partial battle from
being mistaken for a successful test.

The party and link-state probes understand the five-byte WRAM displacement used
by the European Gen I translations, so the same assertions work with the
German Pokémon Blue ROM. The two `*_link_state_localized_final` fields are the
alternate probe values and are retained in the report for diagnosing a ROM
layout mismatch.

For a reproducible scripted run, use `--scenario trade` or `--scenario battle`
with the two Cable Club save states. The scenario enables confirmation input,
starts player one first, delays player two long enough for the host to establish
the link, and stops as soon as the expected outcome is observed. Once both
players reach the Cable Club map, the harness faces them toward the table and confirms
the interaction; the battle scenario then moves both menu cursors to the second
(Colosseum) option before confirming. The trade scenario selects the first
party member on both sides, chooses `TRADE` in the stats menu, and accepts the
trade confirmation after the game's synchronization delay. Scenarios require
`--state1` and `--state2`; this avoids guessing the players' positions from
battery saves. If
the frame budget expires, the report includes `semantic_failure`, menu-seen
flags, and the final localized map markers to identify which phase stalled.

For deeper scripted-run debugging, add `--trace PATH`. The harness writes one
flushed key/value record per emulated frame, including both CPUs' PC/SP and
halt state, serial registers and progress counters, Pokémon link/battle/map
markers, Cable Club menu fields, joypad selection, and (for TCP) request,
response, denial, and waiting counters. The `auto_*` fields show which phase
of the scenario input driver has been reached. A `trace_end` record makes
partial files from a crashed or forcibly stopped run easy to identify. The
trace is opt-in and is never written unless this option is supplied.

For example, a local battle assertion is:

```sh
./build-sdl/gbb_link_harness --transport local \
  --rom roms/pokemon-blue.gb --save1 roms/player1.sav --save2 roms/player2.sav \
  --state1 roms/player1.gbbs --state2 roms/player2.gbbs \
  --scenario battle --frames 1800 --report /tmp/gbb-battle-report.txt
```

To capture the same run for frame-by-frame analysis:

```sh
./build-sdl/gbb_link_harness --transport local \
  --rom roms/pokemon-blue.gb --save1 roms/player1.sav --save2 roms/player2.sav \
  --state1 roms/player1.gbbs --state2 roms/player2.gbbs \
  --scenario battle --frames 1800 --trace /tmp/gbb-battle-trace.log
```

The harness constructs cartridges from memory and imports each save, so the
original `.sav` files remain unchanged. A nonzero exit status means that the
TCP handshake could not be established or a supplied file was invalid.
The semantic failure code distinguishes unchanged party snapshots, an
incomplete trade after the shared menu, and a battle that never started after
both players reached that menu.
