# Shantae: OAM-DMA clock and WRAM return analysis

## Proven first cause

OAM DMA was advanced with `peripheral_cycles` in `MemoryBus::tick`. In CGB
double speed this divides CPU T-cycles by two. Consequently a 160-byte transfer
required 1,280 CPU T-cycles instead of 640, in addition to startup delay. The
HRAM CPU delay loop completed while DMA still blocked WRAM. `RET` therefore
read `FF FF` through the CPU bus even though the physical return word was
intact. This is a DMA clock defect, not corruption of the return address.

[Pan Docs](https://raw.githubusercontent.com/gbdev/pandocs/master/src/OAM_DMA_Transfer.md)
specifies 160 CPU M-cycles for OAM DMA at both speeds: 640 PPU dots normally,
320 in CGB double speed. Its HRAM busy-wait example uses the same 40-iteration
DEC/JR delay pattern. A WRAM stack must remain unused until that transfer ends.

The local fix advances **only OAM DMA** with the original CPU `cycles` value;
PPU and APU clock conversion is retained. DMA startup delay and restart state
are still handled by the existing DMA state machine, now in CPU T-cycles at
both speeds. Normal-speed DMG, CGB and SGB timing is unchanged.

## Local private-ROM evidence

These observations were made with a locally available Shantae cartridge and
are not public reproducible test inputs. No cartridge image, assets, save, or
full CPU trace is included. A temporary read-only probe retained at most 32
HRAM changes, the latest CALL, and whether its stack word changed; the runner
retained at most 64 instruction/stack snapshots.

- At CPU cycles 1,378,032–1,378,356, the copy instruction at bank 1:7D04
  (`LDH [C],A`) installs ten bytes at FF80–FF89, read from bank 1:7D0A–7D13.
  Each written byte matches its ROM source. The routine starts OAM DMA, loads
  A=40, runs `DEC A; JR NZ`, and returns.
- At cycle 2,034,956, the instruction at bank 1:668C intentionally changes
  FF81, the DMA source-high immediate, from C0 to D8. The loop and RET stay
  intact. This selects D800–D89F as the source, with destination FE00–FE9F.
- At cycle 2,167,240, bank 0:0A54 executes `CALL FF80`. SP moves from CFE1 to
  CFDF. The CALL writes high byte `0A` to CFE0 and low byte `57` to CFDF,
  forming return address 0A57. The physical word remains unchanged throughout
  the HRAM routine; that routine performs no WRAM writes and OAM DMA writes
  only OAM.
- At cycle 2,167,928, FF89 executes `RET`. Before the fix, DMA has copied only
  78 bytes. The original-SP raw bytes begin `57 0A`, before and after RET;
  the CPU-visible snapshot is all FF. Both stack reads are still blocked,
  producing PC=FFFF and SP=CFE1.
- At cycle 2,167,944, the CPU fetches IE=03 at FFFF (`INC BC`); the PC wraps
  to 0000. At cycle 2,167,952, the still-blocked ROM fetch supplies FF
  (`RST 38h`). The actual ROM byte at 0000 is C3. The repeated RSTs and later
  I/O/LCDC writes are downstream consequences, not the first failure.
- After the fix, the identical RET starts with 159 bytes copied. DMA finishes
  during its opcode fetch, before either stack read. The RET correctly
  returns to 0A57 with SP=CFE1. A pre-step snapshot still showing `ffff` is
  expected here; it is explicitly labelled and is not an operand-read trace.

The bad-return snapshot can be reproduced on the unfixed clock with the new
read-only diagnostics using `--trace-first-pc 0x0038`. On the fixed clock,
`--trace-first-pc 0x0a57` retains the successful RET and its DMA boundary:

```sh
build-nas/gbb_test_runner /path/to/private/Shantae.gbc --model cgb-e \
  --cycle-frame-capture 600 --frame-output /tmp/shantae.ppm \
  --trace-first-pc 0x0a57
```

For both `--model auto` and `--model cgb-e`, the fixed 600-interval capture
finishes successfully at 42,134,400 CPU T-cycles, PC=0B27, LCDC=E7, with 286
completed PPU frames and 13 RGB colors (`blank_frame=false`). Before the fix
it finished at PC=0038, LCDC=00, five completed frames and one color. An auto
run watching 0038 no longer reaches that address. Auto and forced-CGB capture
files are identical. These are startup checks, not a full gameplay validation.
600 virtual CPU intervals need not produce 600 PPU frames in double speed.

## CPU, memory and mapper audit

- CALL, RST and PUSH decrement SP and write high then low; POP, RET and RETI
  read low then high and increment SP. The CPU read/write helpers advance
  hardware at each memory access. RETI also enables IME. Existing CPU tests
  cover byte order, stack movement, conditions, timing and AF flag masking.
  No stack instruction change was required. See the
  [RGBDS instruction reference](https://raw.githubusercontent.com/gbdev/rgbds/master/man/gbz80.7).
- HRAM is independent of the blocked WRAM source bus. CPU reads use DMA
  arbitration, whereas debugger/raw `read8` bypasses it. The new stack fields
  label both views and their sampling phase explicitly.
- MBC5 keeps the lower ROM window at bank zero and combines low eight and
  high one bank-selection bits for the upper window, including valid bank
  zero. The failing CALL is in the fixed lower window. The selected upper
  bank is 1. Existing mapper tests and new read-only diagnostic checks cover
  bank zero and bit eight. See [Pan Docs MBC5](https://raw.githubusercontent.com/gbdev/pandocs/master/src/MBC5.md).
- FFFF is IE, not ROM. Its value 03 explains the fetched INC BC after the bad
  return; pending interrupt selection masks the five interrupt bits. No IE
  change was justified. See [Pan Docs interrupts](https://raw.githubusercontent.com/gbdev/pandocs/master/src/Interrupts.md).

## Independent regressions and validation

Headless configuration: Ninja, Release, `GAMEBOY_BUILD_TESTS=ON`,
`GAMEBOY_BUILD_SDL=OFF`, built with `--parallel 2` under `build-nas`. The NAS
pipeline and commercial ROM files are untouched.

- `gameboy_cgb_contract`: independently authored CALL into an HRAM DMA loop
  with a WRAM stack. Exercises DMG, SGB1, SGB2, CGB normal and CGB double
  speed. Also checks 159 bytes at 639 CPU T-cycles and completion at 640,
  after the existing eight-cycle startup delay. Before the clock fix, the
  double-speed timing assertions fail and RET reproduces PC=FFFF; afterwards
  all pass. The physical stack word is checked independently.
- `gameboy_cycle_frame_capture`: 17 automated CLI tests, including early RET
  during DMA (CPU-visible FF versus intact raw return), the legitimate final
  DMA/RET boundary, snapshot address wrapping, MBC5 metadata, initial/unreached
  targets, 64-entry wrap and matching register history. Baseline and diagnostic
  runs have identical exit codes, CPU budgets, capture diagnostics and PPM
  bytes; the existing capture, frame and protocol checks remain intact.
- CPU execution/register, PPU timing/DMA, cartridge mapper, save-state and SGB
  PPU contract suites, plus the general core tests, pass.

## Remaining scope

The bus audit also confirms a separate existing limitation: `oam_dma_blocks`
groups cartridge and WRAM accesses together. Pan Docs specifies separate
cartridge and WRAM buses on CGB. This explains why the old failure fetched FF
at ROM address 0000 during WRAM DMA; it does not explain the earlier WRAM
stack reads, which legitimately conflict with WRAM DMA. Correct CGB bus
separation needs its own model-specific regressions and is left as a separate
follow-up. Neither that policy nor DMA start-edge accuracy is claimed fully
hardware-validated by this change. No Windows runtime or physical-hardware
comparison was performed; the shared headless core and owned regressions
validate the demonstrated cause and fix.
