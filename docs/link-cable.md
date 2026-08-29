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

When link diagnostics are enabled, the trace is reset for each local session.
It begins with a `session_start` marker and ends with `session_end`; transfer
counters therefore describe only that session, and frame numbers are written
in decimal for easier correlation with a reproduction.

Network, WebRTC, Bluetooth, and USB transports should be added only after this
deterministic local path is validated with link-enabled games.
