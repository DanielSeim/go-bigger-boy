# Cartridge controllers

GBB dispatches from the cartridge header, never from a game title or hash.
MMM01 has a separate power-on menu header in the last 32 KiB: core loading,
model detection and library metadata use that header when its type and logo
identify it. Validated menu-first MMM01 dumps are normalized in memory before
mapping; the source file and its fingerprint remain unchanged. See the
[MMM01 investigation](mmm01-dump-layout-analysis.md) for detection rules,
local private-ROM evidence and bounded diagnostics. An ordinary header remains
at offset zero. No commercial ROMs or
saves are part of the new tests.

## Implementation status

Successful loading is not evidence of complete game compatibility. A local
Taito Variety Pack menu and game-selection replay has now been checked; the
other private NAS cartridges were not revalidated during this investigation.

| Controller | Implemented and covered by synthetic contracts | Remaining limits / unknown behavior |
| --- | --- | --- |
| MMM01 (`0B`–`0D`) | Last-two-bank startup, 9-bit ROM addressing, ROM/RAM masks, irreversible mapping, mode lock, multiplex mapping, 16 RAM banks, battery RAM for `0D` | Pre-map RAM access and some simultaneous mask/map writes lack hardware measurements. Enabled pre-map RAM uses the selected bank; masks apply on later bank writes. Initial upper-window low bits are modeled as the last ROM bank. |
| MBC7 (`22`) | Seven ROM-bank bits, dual register enables, sensor erase/latch sequence, deterministic neutral input, 128 × 16-bit serial EEPROM, READ/sequential READ/WRITE/ERASE/EWEN/EWDS/WRAL/ERAL, write protection, busy state, persistent EEPROM | Rumble is not implemented: no verified control pin/register mapping was found; MBC5 rumble bits are not assumed. Physical sensor settling/noise and host sensor bindings are absent. Bank zero is allowed, pending hardware confirmation. Programming takes a fixed 5 ms model rather than a measured variable duration. |
| TAMA5 (`FD`) | Unlock/select/data interface, five ROM-bank bits, 32-byte memory, RTC pages and masks, documented minute/hour/start/stop/alarm-flag commands, valid BCD calendar with 12/24-hour mode and independent leap counter, persistence | MCU caching/timing, undocumented commands (including `43`), invalid-BCD carry behavior and audible alarm output are not implemented. Invalid calendar data is retained; clock advancement pauses until valid. Unsupported reads return open bus. Power-on upper ROM bank 1 and zeroed memory/clock values are deterministic emulator defaults, not measured battery contents. |
| HuC3 (`FE`) | Seven ROM-bank bits, four RAM-bank bits, read-only/read-write modes, nibble mailbox/address commands, clock copy/set/status, 12-bit day counter and minute/subminute arithmetic, event-time adjustment, persistence, IR pins | MCU commands finish synchronously; physical MCU timing, speaker commands/tone generation and unknown commands remain unimplemented. Unknown commands retain the prior response. IR has no device-to-device transport. |
| HuC1 (`FF`) | Six ROM-bank bits including bank zero, independent two-bit RAM banking, always-accessible RAM outside exact IR mode `0E`, IR receiver/transmitter pins, battery RAM | IR has no frontend or device-to-device transport. |

Ordinary ROM-only, MBC1/MBC1M, MBC2, MBC3, MBC5 and Camera paths remain in
place. Extra state is allocated only for the five additional controllers.
ROM and SRAM access reuse the existing cartridge bank/storage helpers.
CPU, PPU and APU instruction/render/audio logic is unchanged. The memory bus
advances MBC7 programming in base-clock cycles, including in CGB double speed.

## External inputs and time

`Cartridge::set_accelerometer(x, y)` supplies raw encoded MBC7 samples; neutral
is `81D0` on both axes. The emulated CPU must still issue erase `55` followed
by latch `AA`. The erased latch reads `8000`. No frontend sensor binding is
claimed: without one, games receive deterministic neutral tilt.

`set_infrared_input(light)` and `infrared_output()` expose separate receive
and transmit pins. Disconnected IR means no received light.

`set_rtc_clock(callback, context)` provides deterministic seconds for tests;
the default is the system clock. Host callbacks and context pointers are never
serialized. HuC3 wraps its day counter after 4096 days. TAMA5 advances valid
calendar values with a four-year leap cycle and a two-digit year; long gaps are
reduced to a bounded calendar calculation. Save-state restore freezes the clock
at the saved emulated time and reanchors it to the current host time, matching
existing MBC3 state behavior. Battery RTC imports account for offline time.

## Persistence and save-state compatibility

Existing MBC1/2/3/5 `.sav` files and MBC3 `GBBRTC1` sidecars are unchanged.
New storage uses the existing save path and explicit byte serialization:

- MMM01/HuC1/HuC3: raw SRAM `.sav` (only battery-capable types persist).
- MBC7: 256 EEPROM bytes, high byte first for each 16-bit word, in `.sav`.
- TAMA5: 32 command-memory bytes in `.sav`.
- HuC3/TAMA5: 339-byte `.rtc`, magic `GBBXRTC` followed by format byte `1`,
  controller type, little-endian 64-bit timestamp, 256 HuC3 nibbles, subminute,
  64 TAMA5 nibbles and a timer-running Boolean. Type, size, nibbles, Boolean and
  timestamp are validated before accepting an imported sidecar. This is a GBB
  format; importing other emulators' RTC or EEPROM files is not claimed.

Save-state version **43** appends a typed controller extension after existing
fields. States from supported earlier versions still load for existing
controllers; a pre-43 state for a newly supported controller is rejected
explicitly because it cannot contain that controller's state. Malformed
extensions reject and roll back through the existing atomic load mechanism.
Bank locks, command selection, sensor latches, IR pins, RTC data and partially
transferred or busy EEPROM operations survive roundtrips. Version 43 states
cannot be read by older GBB binaries.

## Test coverage

`gameboy_cartridge_extra_contract` uses independently constructed bank-marker
ROMs and externally specified protocol expectations. It covers mapping/masks/
locks, invalid/disabled interfaces, serial EEPROM edges/write protection/
abort/sequential wrapping, save/load during an EEPROM word and during busy,
injected sensor input, RTC midnight/leap-day/stop behavior, IR pins, all five
controllers' file persistence and state roundtrips, automatic DMG/CGB startup,
invalid-state rollback and version-42 MBC1/2/3/5 compatibility.

`gameboy_cartridge_header_inspection` covers offline inspection, invalid
headers, internal memory sizes, MMM01 menu offsets and read-only operation.
The ROM-library contract also checks menu metadata. Existing mapper, save-state,
bus, CPU, LCD and core contracts provide regression coverage. These checks do
not substitute for hardware measurements or the private NAS game suite.

## Synology validation (offline)

Use Python **3.8 or newer**. The script only reads local files and prints JSONL;
it neither writes ROMs nor imports network clients. Run it on the NAS itself:

```sh
python3 scripts/inspect_cartridge_headers.py --recursive /volume1/roms/regression > /tmp/cartridge-headers.jsonl
python3 scripts/inspect_cartridge_headers.py --expected-type 0x22 /volume1/roms/path/to/tilt-game.gbc
```

Output includes header offsets, type, CGB flags, size codes, effective internal
memory size, logo/header/global-checksum checks, errors and warnings. Exit 1
means an error was found; warnings are retained separately. `--expected-type`
compares against operator-supplied board/catalog metadata. Headers alone cannot
verify which game or board a dump actually belongs to, so
`game_identity_verified` remains false. Do not use the diagnostic to patch ROMs
or silently override controllers.

Build and test without SDL:

```sh
cmake -S . -B build-nas -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=OFF
cmake --build build-nas --target gameboy_test_runner gameboy_cartridge_extra_contract_tests --parallel 2
ctest --test-dir build-nas --output-on-failure -R 'gameboy_cartridge_(extra_contract|header_inspection)'
```

Then rerun the existing 100-ROM regression suite with its original capture
intervals and compare all 95 previously passing cases as well as the five new
loads. Inspect capture diagnostics, framebuffer and save/reload behavior;
a successful load alone is insufficient. For MBC7 separately exercise EEPROM
save/reload and neutral tilt; for RTC games check time progression and reload.
Keep ROMs, saves, titles/paths if sensitive, and framebuffer captures private.

## References and evidence boundaries

Protocol descriptions used for independent implementation:

- [Pan Docs MMM01](https://gbdev.io/pandocs/MMM01.html): startup, mapping, masks and documented unknowns.
- [Pan Docs MBC7](https://gbdev.io/pandocs/MBC7.html): register enables, tilt encoding and serial EEPROM commands.
- [Pan Docs HuC1](https://gbdev.io/pandocs/HuC1.html): always-accessible RAM, six-bit ROM bank and IR mode.
- [Pan Docs HuC3](https://gbdev.io/pandocs/HuC3.html): mailbox commands, clock memory and modes.
- [GBCTR](https://gekkio.fi/files/gb-docs/gbctr.pdf): controller address wiring, including HuC3 RAM bank width.
- [TAMA5 hardware investigation by endrift](https://gbdev.gg8.se/forums/viewtopic.php?id=469): command protocol, TC8521AM pages/masks, corrected RAM direction and observed unknowns.

Public [mGBA mapper sources](https://github.com/mgba-emu/mgba/tree/master/src/gb/mbc)
were inspected as a secondary cross-check, including `licensed.c`, `mbc.c`,
`tama5.c` and `huc-3.c`. GBB's added source is independently written; reference
source and private cartridge contents are not bundled. Synthetic contracts
validate the modeled interfaces, not every undocumented hardware behavior.

## Local validation record (2026-10-12)

- Required Ninja Release/headless configuration and the complete configured
  native build succeeded; the final runner and new mapper-contract target
  were rebuilt after the controller changes.
- 29 distinct selected CTest checks passed: header inspection, cycle capture,
  core/library, cartridge file/new mapper/existing mapper, save format/state,
  CPU register/ALU/execution, timer/STAT/PPU, three APU contracts, hardware model
  matrix, CGB, SGB PPU/sound and six boot/presentation checks.
- New and existing mapper contracts passed with AddressSanitizer and
  UndefinedBehaviorSanitizer (including LeakSanitizer). The sandbox's ptrace
  monitoring prevented LeakSanitizer startup; repeating outside that sandbox
  passed without disabling leak checks.
- All eight changed production translation units passed MinGW C++17 Windows
  syntax compilation. A native Windows application build/runtime was not run.
- The complete long SGB firmware matrices, external conformance collections,
  real cartridge hardware and the private 100-ROM NAS suite were not run for
  this mapper change. No failures were suppressed or converted to success.
