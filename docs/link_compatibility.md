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

Profiles also include a region encoding. Japanese (and future Korean) builds
must not be mixed with Western builds because their text and party encodings
are different. Older GBB peers do not send the extension; they continue to
use the legacy compatibility ID and can still connect to current builds when
that ID matches.

The emulator's serial implementation models SB/SC and CGB clock selection,
including the double-speed clock bit. Pokémon's retail protocol normally
selects the standard serial clock; no fast mode is forced by the transport.
Time Capsule data conversion (species indices, held-item/catch-rate bytes,
party layouts, and patch lists) remains in the Pokémon ROM, matching the
original hardware division of responsibility.

For a mixed Gen I/Gen II session, the packet transport deliberately uses
bit-level serial packets instead of its optional byte packet shortcut. The
two games run different interrupt and transfer state machines during the Time
Capsule exchange; retaining each cable edge prevents one side from advancing
an entire byte while the other side is between serial interrupts. Same-family
sessions may continue to use the byte shortcut after profile negotiation.

The protocol behavior was cross-checked against the pret disassemblies:
[pokecrystal link code](https://github.com/pret/pokecrystal/tree/master/engine/link)
and [pokered Cable Club code](https://github.com/pret/pokered/blob/master/engine/link/cable_club.asm).
