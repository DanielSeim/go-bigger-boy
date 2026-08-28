# Link cable milestone

The core now models the Game Boy serial port at bit granularity. `SB` (`FF01`)
is shifted one bit per clock edge and `SC` (`FF02`) controls transfer start,
clock ownership, and CGB fast mode. DMG transfers use 512 CPU clocks per bit
(8192 Hz); CGB fast mode uses 16 clocks per bit.

`gameboy::SerialCable` connects two `gameboy::SerialPort` instances without
threads or I/O. The console selecting the internal clock drives both ports,
while the other port receives the same edges as an external-clock device. A
missing endpoint supplies pull-up `1` bits, matching the disconnected cable
state. Transfer completion raises serial interrupt 3 on both consoles.

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
single-console view and reconnects cleanly when a new ROM is loaded.

Network, WebRTC, Bluetooth, and USB transports should be added only after this
deterministic local path is validated with link-enabled games.
