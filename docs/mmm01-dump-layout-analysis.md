# MMM01: menu-first dump ordering and bounded startup diagnostics

The first incorrect association in the locally tested Taito Variety Pack dump
was present at startup, before any mapper-register write. Its file starts with
the menu, while the hardware mapper boots the last 32 KiB. GBB treated that
file as physical chip order and executed its empty trailing banks instead.

## Mapper review

The register implementation is retained. These are code-review and synthetic
contract findings, not a claim of exhaustive hardware validation:

| Area | GBB behavior and evidence |
| --- | --- |
| Reset | Four zeroed 7-bit registers, unmapped; ROM windows select the last two banks. For 32 banks those are 30/31. |
| MMM01 versus MBC1 | Extended game-selection bits, ROM/RAM masks and multiplex selection precede an irreversible mapping transition; ordinary MBC1 switching alone cannot model that startup. |
| ROM selection | The production selectors combine high/middle/low fields, preserve masked bits and apply the upper-window zero-bank alias without modifying the raw latch. |
| RAM selection | The production selector handles RAM masks, mode and multiplex; RAM enable remains independently writable. A ROM-only cartridge has no RAM even if the enable latch is set. |
| Locks | Mapping bit 6 freezes extended controls; mode-disable independently prevents mode changes. Writes to locked fields are still observable accesses. |
| Smaller images | Actual ROM reads wrap requested banks by the declared physical bank count. The new snapshot exposes both requested and effective banks. The 32-bank contract exercises a requested bank above 400. |
| Lower-window execution | The next instruction fetch uses the newly selected lower bank immediately. A synthetic program switches from the menu to bank 8 and continues at the same CPU address. |
| States | Version 43 already serializes all four raw registers and RAM enable. No new serialized fields or version change are needed for diagnostics or file-order normalization. |

[Pan Docs MMM01](https://gbdev.io/pandocs/MMM01.html) describes menu-last startup
and the control fields. [SameBoy's cartridge setup](https://github.com/LIJI32/SameBoy/blob/master/Core/mbc.c)
explicitly rotates menu-first files before using the hardware mapping. That
supports a loader correction rather than changing the mapper's reset bank.

References are not interchangeable hardware oracles. The inspected SameBoy
mapping code uses a different multiplex-mode lower-window condition and
identical branches for the nonmultiplex RAM-mode selection; its
[write handling](https://github.com/LIJI32/SameBoy/blob/master/Core/memory.c)
also differs in RAM-low mask handling. The inspected
[mGBA implementation](https://github.com/mgba-emu/mgba/blob/master/src/gb/mbc/licensed.c)
has incomplete mode/mask handling and locks on the first register-region-zero
write. Those differences were not copied into GBB. Hardware measurements are
still needed for pre-map RAM, the suspected pre-map upper-window low-bit
behavior, and simultaneous RAM-mask/mapping writes. They do not explain a
failure before the first mapper write.

## Correction and compatibility

A recognizable trailing MMM01 menu header retains priority, preserving the
existing physical-order path. Otherwise, an image larger than 32 KiB whose
first header has MMM01 type, the standard logo, a valid header checksum and an
exact matching declared size is interpreted as menu-first. The first 32 KiB
are rotated to the end **in the cartridge's in-memory vector**, with no second
ROM-sized buffer. No game name or hash selects behavior. Invalid or
unrecognized first headers do not trigger normalization.

Library/model detection can still read the original first menu header. The
offline inspector reports `dump_layout`, original `header_offset` and
`normalized_header_offset`; it never rewrites the file.

The fingerprint is calculated from the original file bytes before rotation;
battery paths, RAM format and state identity remain stable. Physical-order
MMM01 and MBC1/3/5 states retain their mapping. Previously saved states from a
misinterpreted menu-first dump remain structurally readable but restore CPU
state against the corrected ROM order: they cannot repair the old invalid
execution. Restart such a game. States made with the corrected loader reload
into a fresh cartridge with the same source image and reproduce exact bytes.

## Diagnostics

All options are off by default and work independently or together, including
with the existing CPU first-PC/recent-history options. Storage is allocated
only when one of these diagnostics is requested. CPU/PPU/APU execution logic
is unchanged; a host-only memory-bus callback brackets accepted CPU control
writes. DMA-blocked writes are excluded. Direct debugger writes are excluded.

- `--trace-mmm01 PATH` creates a file only for MMM01. `MMM01_STATE` records reset
  banks and loader layout (`as_loaded` or `menu_first_normalized`).
  `MMM01_FETCH_FIRST` records the first actual opcode fetch. `MMM01_WRITE`
  records each accepted register access, including repeated values and masked
  writes, with responsible instruction PC, address/value, exact write cycle,
  CPU registers at the write, and before/after raw registers, masks, mode,
  locks, requested/effective banks and RAM enable/presence. Bank calculations
  come from the cartridge's existing selectors. A two-write instruction gets
  two records. An interrupt push has `pc=none transition=interrupt`.
- `--trace-nop-run N` reports the first run of N executed opcode-zero
  instructions once. `NOP_RUN`, `NOP_RUN_START`, `NOP_RUN_CURRENT`,
  `NOP_RUN_HISTORY` and `NOP_RUN_MMM01_WRITE` distinguish the sequence's start,
  threshold instruction, up to 64 instructions **before** its start and up to
  32 recent mapper writes. Idle and interrupt-only steps break a run.
- `--trace-first-lcd-off` reports the first accepted LCDC bit-7 falling edge.
  `LCD_OFF` contains exact write, step-start and instruction-end cycles.
  `LCD_OFF_WRITE` contains write-boundary registers and mapping;
  `LCD_OFF_INSTRUCTION` contains completed-instruction registers.
  `LCD_OFF_HISTORY` includes up to 64 instructions through that instruction;
  `LCD_OFF_MMM01_WRITE` retains up to 32 recent mapper accesses.

`cycle` uses CPU T-cycles. For writes it is the step's CPU cycle plus the bus
clock delta inside that step, including at CGB double speed. Instruction
history uses step-start cycles and explicit durations. Hexadecimal PC/opcode/
register fields have no prefix; cycle and bank counts are decimal; Booleans
are literal `true`/`false`. `boundary=cpu_write` must not be mistaken for
instruction-end state. A legitimate `RST 38h` is reported without automatic
error classification.

Fetch records include the **actual fetched opcode**, cartridge byte bypassing
boot/DMA, raw bus byte, CPU-visible bus byte, DMA/boot flags and whether the
opcode matches ROM. Read-only comparison samples are taken just after opcode
fetch, not advertised as separate historical bus transactions. For MMM01,
`physical_offset` identifies the normalized chip byte and `file_offset` the
corresponding original file byte. Boot overlays, non-ROM execution and DMA
can explain discrepancies; equality alone does not prove the source.

Mapper-file output has a hard ceiling of 4,096 writes, optionally reduced by
`--trace-limit N`, then one `MMM01_TRUNCATED` marker. Recent-write/history rings
continue after truncation. NOP and LCD triggers each fire once and retain
fixed-size arrays regardless of N. The CPU history is never written as a full
trace to disk; mapper output is only a few MiB at the ceiling. Private opcode
histories and screenshots stay local and must not become CI artifacts.

## Local private-ROM evidence

These runs used the locally available original source file, not the regression
copy or alternate bad dumps. Source SHA-256 was recorded privately before
work and checked afterward. No ROM, screenshot, instruction-history file or
private save is included in the repository. The results are local evidence;
public reproduction uses the authored fixtures below.

Before normalization, diagnostics found:

- The first actual instruction at CPU PC `0100` fetched opcode zero from
  effective bank 30, file offset `491776`; bank 31 was also empty. The first
  256-NOP run starts immediately there, with no preceding CPU history.
- Windows remained at 30/31 throughout the first 60 virtual intervals. There
  were **zero mapper writes** through cycle `12059516`, and still none through
  the 300-interval capture. Registers stayed zero/unmapped.
- At step-start cycle `12059500`, PC `0038` fetched `FF` during active DMA,
  differing from the underlying ROM byte. `RST 38h` moved SP from `FF42` to
  `FF40`; pushing return address `0039` wrote `39` to LCDC at cycle `12059516`.
  The completed PC was again `0038`. There was no neighboring bank switch.
  Thus the LCD-off event is a stack-write consequence of the earlier invalid
  startup, not evidence that the menu intentionally disabled its display.

| Virtual intervals | Before: frames / colors / LCDC | Corrected: frames / colors / LCDC |
| --- | --- | --- |
| 60 | 60 / 1 / `91` | 56 / 4 / `91` |
| 300 | 171 / 1 / `39` | 292 / 4 / `93` |
| 600 | 171 / 1 / `39` | 592 / 4 / `93` |

After normalization, the first LCD-off edge is at cycle `417300`, with LCDC
`91` to `11`, followed by later re-enabling. Visual inspection at 600 intervals
shows the compilation title and START prompt. No mapper writes are expected
while waiting on that screen.

A separate input replay pressed START at completed PPU frame 300, released at
310, pressed A at 500 and released at 510. It reached a mapping handoff at
cycle `39534964`, executing the write from HRAM PC `FF97`: banks 30/31 became
16/17. At 1,200 virtual intervals it had 1,172 completed frames, four colors
and LCDC `E7`. Visual inspection shows the Bubble Bobble title screen. This demonstrates
menu-to-game switching and continued video,
not full playthrough coverage of every game or hardware-equivalent sound.

## Public regression evidence

The independently authored menu-first/menu-last bank-marker test was run
against the previous loader and failed at startup and subsequent game-bank
comparisons. With normalization it passes. Additional contracts cover header
validation and trailing-menu priority, 32-bank wrapping, raw-latch snapshots,
read-only state identity, both-layout state roundtrips, loading into a fresh
cartridge, RAM persistence and legacy mapper state compatibility.

`gameboy_mmm01_diagnostics` exercises independent/combined options, exact
multi-write timing, equal-value writes, ignored/locked controls, lower-window
handoff, DMA-blocked writes, bounded history/output after truncation,
pre-sequence NOP history, HALT idles, malformed CLI arguments, output errors,
unchanged captures and unchanged memory snapshots. The C++ observer contract
also compares complete serialized emulator states with/without the observer
at normal and double CPU speed.

Validation completed in the headless Release build (`GAMEBOY_BUILD_TESTS=ON`,
`GAMEBOY_BUILD_SDL=OFF`): 12 selected CTest groups passed, covering the new
CLI, cycle captures, boot CLI, header/file/extra/existing mapper contracts,
CPU/CGB and state-format/state-load/save-sync contracts. ASan/UBSan runs of the
new diagnostics, cycle captures and extra-controller contracts passed.
MinGW C++17 syntax checks passed for the runner, cartridge, extra mapper,
memory bus and extra-controller tests; a native Windows runtime was not run.

## Synology commands

Keep outputs in a private directory. Substitute local paths; no network or
external firmware is needed for this DMG investigation:

```sh
cmake -S . -B build-nas -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-nas --target gameboy_test_runner gameboy_cartridge_extra_contract_tests --parallel 2
ctest --test-dir build-nas --output-on-failure -R 'gameboy_(mmm01_diagnostics|cartridge_extra_contract|cycle_frame_capture|cartridge_header_inspection)$'

mkdir -p /tmp/gbb-mmm01
python3 scripts/inspect_cartridge_headers.py --expected-type 0x0b '/volume1/roms/Taito Variety Pack (J).gb'
build-nas/gbb_test_runner '/volume1/roms/Taito Variety Pack (J).gb' \
  --cycle-frame-capture 600 --frame-output /tmp/gbb-mmm01/capture.ppm \
  --trace-mmm01 /tmp/gbb-mmm01/mapper.log --trace-limit 4096 \
  --trace-nop-run 256 --trace-first-lcd-off \
  2>/tmp/gbb-mmm01/diagnostics.log
```

For the selection replay, create a private text file `select.input`:

```text
GBB SGB input v1
300 start
310 none
500 a
510 none
```

Run the same command with `--cycle-frame-capture 1200` and
`--input-script select.input`. Input timestamps count completed PPU frames,
not virtual cycle intervals. No new full CPU trace is necessary.
