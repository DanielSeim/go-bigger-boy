# DMG replacement boot: serial validation

Revision 4 aligns the replacement's idle serial phase with local opaque
execution of the original boot on GBB's unchanged cold baseline. This is a
same-core contract, **not** proof of physical reset/serial-clock equivalence.
The original cold path's four-clock DIV discrepancy remains unresolved.

## Findings and correction

Before the change, replacement handoff phase was 392 of 512 T-cycles; the cold
reference was 452. With an immediate standalone internal-clock start, the first
edge therefore occurred after 120 versus 60 clocks, and the eighth edge after
3,704 versus 3,644 clocks. Both shifted valid bytes, but the first-transfer
timing differed by 60 clocks.

The replacement now clears SC at clock 36 rather than 96. Moving that ordinary
CPU write earlier, adding one NOP and using a 12-clock `LDH` instead of the
16-clock IE write preserves the total 96-clock initialization prefix. Later
APU initialization, startup duration (4,322,792 clocks), DIV=`ABC8`, LCD dot 396
and inherited APU state are unchanged. No private serial phase is injected,
no cold-clock offset is applied, and no original boot instructions are read.
Both header-checksum paths leave phase 452 with no transfer active.

The production DMG/MGB post-boot profile remains at its existing phase 460.
That profile is distinct from executed cold boot; this work does not claim
their first standalone transfers are identical. Other models' startup timing
and the shared serial clock implementation are unchanged.

An attached cable deliberately resets its phase when armed and waits for an
unarmed peer. Its next-edge policy remains unchanged, including a held edge
one clock after the late peer arms. A cable attached before replacement boot
retains its idle held boundary rather than free-running to phase 452. These
are current GBB transport contracts, not newly asserted hardware behavior.

### External-edge save bug

The new checks also reproduced corrupted partial external transfers when a
save was taken immediately after a peer edge, before the receiving bus's next
tick. Guest SB reads already used the live serial shift register, but saves
wrote its stale backing I/O cache. The writer now captures live SB/SC into the
existing I/O fields. The payload size/version and legacy load paths are
unchanged. This fixes newly written states; it cannot recover already lost
bits in an older malformed save.

Regressions cover saving after each of bits 1–7 for DMG, CGB-E, SGB and SGB2,
then comparing remaining outgoing bits, received bytes and interrupt timing.
No clock rates, SGB renderer/audio behavior or link transport are changed.

## Reproduce the local comparison

```sh
python3 scripts/compare_dmg_boot_serial.py \
  --probe build/gbb_dmg_boot_probe \
  --reference-boot /private/path/dmg_boot.bin \
  --rom /private/path/cartridge.gb \
  --output /tmp/new-dmg-serial-report.json
```

Repeat `--rom` for more titles. The probe is test-only, not installed with
releases. It executes the local reference as an opaque input, saves handoff
state **in memory**, and runs isolated clones using host bus writes/ticks.
No cartridge instructions execute during these diagnostics. They do not
represent gameplay, network round trips or CPU instruction-write timings.
The original emulation's handoff/followup/PCM are unaffected by `--serial-check`.

Reports include input/tool hashes and only diagnostic bit events from fixed
GBB-selected bytes `A5` and `3C`; they never include ROM, VRAM, WRAM, raw
save-state payloads or cartridge audio. No adjacent battery/RTC files are
loaded or written. Existing reports are not overwritten, changed inputs/tools
are rejected, and cold-clock offsets are disallowed by the comparator.

Each run is checked independently against expected bit/data/SC/IF behavior;
matching-but-broken traces cannot pass. The comparison covers:

- Ten idle intervals, including just before/at a divider wrap, first edge,
  all eight 512-clock edges and serial interrupt only on edge eight.
- Disconnected pull-up reception, single output publication, abort/rearm and
  mid-byte internal save restoration between edges.
- External waiting, irregular peer edges, MSB-first exchange and restoration
  immediately after the third edge.
- Mixed executed-boot/default-post-boot pairs in both master roles, ready and
  late peers, exchanged bytes and exactly one completion per console.

Exit 0 means the stable boot and serial contracts match and both protocol
checks pass. Exit 1 records a mismatch; exit 2 indicates invalid input or a
tool failure. Neither a pass nor the phase correction resolves hardware reset
provenance or validates a Pokémon trading/battle session.

Local checks on 2026-10-04 pass for Pokémon Blue (UE), Super Mario Land v1.1,
Tetris v1.0 and Donkey Kong (JU) v1.1: phase 452, first standalone edge at 60
clocks, completion at 3,644 clocks. Every bit, interrupt and linked case matches.

The final optimized suite completed with 160 passing tests and three
network-related skips (163 entries, 647.47 seconds). All ten focused checks
passed, including eight Python serial regressions; focused ASan/UBSan checks
also passed with LeakSanitizer disabled for the sandbox. Mooneye boot-register,
divider and boot-I/O tests and Blargg CPU/instruction/memory timing tests passed
through replacement boot. All four 30-second audio comparisons passed, with
replacement PCM hashes unchanged from revision 3, and their completed-frame
hashes after 60 million followup clocks were unchanged too. SGB title/PCM
baselines passed in the full suite. These are the captured scenes and local
test conditions, not a claim of exhaustive gameplay or hardware equivalence.

## Offline tests

```sh
ctest --test-dir build \
  -R 'gameboy_dmg_|gameboy_serial_link_contract|gameboy_hardware_model_matrix_contract' \
  --output-on-failure
```

The new Python CTest entry uses only logo-free synthetic headers and GBB's
generated firmware as its local fixture, not Nintendo inputs. Mutation tests
detect timing/data/interrupt changes, restore failures, missing cases, offset
concealment and equally incorrect traces. CPU-executed synthetic homebrew
also arms SC, wakes from HALT, services the serial ISR exactly once and resumes
identically after a partial-byte save, with default and replacement startup.

The serial register, shift and interrupt contracts follow public
[Pan Docs serial documentation](https://gbdev.io/pandocs/Serial_Data_Transfer_(Link_Cable).html).
See also [boot validation](dmg-boot-validation.md) and
[cartridge audio validation](dmg-boot-audio-validation.md).
