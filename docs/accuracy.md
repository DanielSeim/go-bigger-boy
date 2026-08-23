# Accuracy status

The automated baseline uses the pinned
[`c-sp/game-boy-test-roms` v7.0](https://github.com/c-sp/game-boy-test-roms/releases/tag/v7.0)
bundle. GitHub Actions verifies the archive checksum before running any ROM.

## Current automated baseline

| Suite | Passing | Coverage |
| --- | ---: | --- |
| Mooneye acceptance | 75/75 | Complete acceptance directory, with model-specific boot profiles |
| Blargg | 14 | All 11 individual CPU instruction ROMs plus instruction and memory timing |
| Total CI gate | **89** | Every listed ROM must pass before a release can be published |

The Mooneye figure covers every acceptance ROM in the pinned bundle. Tests with
mutually exclusive boot-ROM expectations run under explicit DMG0, DMG/MGB,
SGB, or SGB2 post-boot hardware profiles.

## Next accuracy work

With acceptance coverage complete, the next accuracy milestone is expanding
the automated gate to Mooneye's emulator-only and manual-test groups, then
adding broader CGB-specific timing suites.

Run the exact CI baseline locally with:

```sh
cmake -S . -B build-conformance \
  -DGAMEBOY_BUILD_SDL=OFF \
  -DGAMEBOY_TEST_ROM_DIR=/path/to/game-boy-test-roms-v7.0
cmake --build build-conformance
ctest --test-dir build-conformance -L conformance --output-on-failure
```
