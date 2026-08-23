# Accuracy status

The automated baseline uses the pinned
[`c-sp/game-boy-test-roms` v7.0](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0)
bundle. GitHub Actions verifies the archive checksum before running any ROM.

## Current automated baseline

| Suite | Passing | Coverage |
| --- | ---: | --- |
| Mooneye acceptance | 50 | CPU timing, interrupts, OAM DMA, timers, and selected PPU timing |
| Blargg | 14 | All 11 individual CPU instruction ROMs plus instruction and memory timing |
| Total CI gate | **64** | Every listed ROM must pass before a release can be published |

The Mooneye figure is 50 of the 75 acceptance ROMs in the pinned bundle. Tests
that target a different boot-ROM hardware revision are counted during the full
audit but are not necessarily applicable to GBB's current post-boot DMG model.

## Next accuracy work

The remaining Mooneye acceptance gaps are grouped here so that newly fixed ROMs
can be moved into `cmake/ConformanceTests.cmake` immediately:

- Boot state and model-specific unused I/O behavior: 12
- Interrupt-entry `IE` stack-write interaction: 1
- PPU mode, LCD-enable, STAT/LYC, and VBlank timing: 8
- Serial boot-clock alignment: 1
- Timer reload and rapid-toggle edge cases: 3

Run the exact CI baseline locally with:

```sh
cmake -S . -B build-conformance \
  -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_TEST_ROM_DIR=/path/to/game-boy-test-roms-v7.0
cmake --build build-conformance
ctest --test-dir build-conformance -L conformance --output-on-failure
```
