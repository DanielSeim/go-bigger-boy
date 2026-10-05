# Pokémon link compatibility

GBB keeps the Game Boy serial bus model independent from Pokémon's
software-level link protocols. The ROM remains responsible for the actual
Cable Club/Trade Center/Time Capsule byte exchange; the transport handshake
only prevents an obviously incompatible peer from starting that exchange.

The handshake carries an optional `LinkCompatibilityProfile` extension:

| generation pair | protocol selected by Pokémon |
| --- | --- |
| Gen I + Gen I | Gen I Cable Club |
| Gen II + Gen II | Gen II Cable Club/Trade Center |
| Gen I + Gen II | Gen II Time Capsule bridge |

Profiles also include a region encoding; different known regions are rejected.
Cartridge inference recognizes Pokémon title prefixes and treats destination
byte `0x14A == 0` as Japanese and other values as Western. The region enum
includes Korean, but automatic inference does not identify it separately.
This is a header heuristic, not proof of language, retail identity, or gameplay
compatibility: hacks retaining recognized headers can inherit their profile.
Unrecognized titles fall back to their exact ROM fingerprint.

Older peers using the same packet framing may omit the optional profile
extension; they continue to use the legacy compatibility ID when it matches.
This does not provide compatibility with unsupported packet protocol versions.
Known profiles check version, region and supported modes, allowing Gen I/II
entry only when the Gen II profile advertises Time Capsule. The ROM still
decides whether the selected room and party are valid for the operation.

The emulator's serial implementation models SB/SC and CGB clock selection,
including the double-speed clock bit. Pokémon's retail protocol normally
selects the standard serial clock; no fast mode is forced by the transport.
Time Capsule data conversion (species indices, held-item/catch-rate bytes,
party layouts, and patch lists) remains in the Pokémon ROM, matching the
original hardware division of responsibility.

The packet transport keeps its negotiated byte representation for both
same-generation links and mixed Gen I/Gen II Time Capsule entry. The guest ROMs
perform the generation-specific conversion and pacing; changing transport
granularity during the initial Cable Club handshake can leave the two games in
different reserved/waiting states. Deferred requests are bounded separately so
a peer that stops arming its receiver can recover instead of holding a packet
forever.

The protocol behavior was cross-checked against the pret disassemblies:
[pokecrystal link code](https://github.com/pret/pokecrystal/tree/master/engine/link)
and [pokered Cable Club code](https://github.com/pret/pokered/blob/master/engine/link/cable_club.asm).

That historical source comparison is distinct from current end-to-end
qualification. Local profile/packet contracts check negotiation and framing;
the headless `gbb_link_harness` accepts a single ROM for both players and cannot
validate a mixed-ROM Time Capsule pair. Record a real two-peer trade result
before claiming a specific language/version pair works. Web has no link
transport; native TCP is available on desktop/Android and Bluetooth Classic
RFCOMM is implemented on Windows/Android.
