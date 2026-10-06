# Original SPC700 IPL replacement

GBB bundles an independently written SPC700 port-protocol loader for the
experimental SGB firmware host. The desktop firmware directory needs only the
selected SGB1/SGB2 SNES program ROM. An optional, exactly 64-byte `spc700.rom`
overrides the loader; malformed or unreadable overrides are rejected. The
Game Boy bootstrap overrides remain optional as well.

## Source and reproducibility

[`firmware/spc700/ipl.asm`](../firmware/spc700/ipl.asm) and its generated
`ipl_image.hpp` use the repository's GPL-3.0-or-later license. The small strict
assembler in `scripts/build_spc700_ipl.py` encodes the source's SPC700 instruction
subset and resolves relative branches. It reads no reference image, needs only
Python, rejects oversized images and pads unused bytes with NOPs. Normal C++
builds consume the checked-in header without Python or an external assembler.
The header records normalized source and image SHA-256 hashes.

```sh
python3 scripts/build_spc700_ipl.py --check
python3 scripts/build_spc700_ipl.py --check --output NEW_IMAGE.bin
```

The firmware implements the publicly documented
[S-SMP port loading protocol](https://snes.nesdev.org/wiki/S-SMP): AA/BB readiness,
CC first command, address/mode latching before acknowledgment, byte counters,
page rollover, new blocks and indirect entry. Development uses execution-only
observations of the private original for port timing and behavior. No private
image is decoded or read by the assembler, and no reference image, uploaded
SGB driver, audio capture or game data is distributed.

The supported entry contract is GBB's SPC reset at PC=FFC0 with PSW.P=0.
The loader clears RAM 01..EF, establishes SP=EF, leaves I/O untouched until
the ready writes, and uses 00/01 as the transfer pointer. Entry into the uploaded
program has A=X=Y=0 and SP=EF. It does not disable the IPL overlay; uploaded
code controls that through the ordinary CONTROL register. Code that depends
on instruction addresses inside the private IPL, rather than the port protocol
and FFC0 entry, is outside this replacement's compatibility claim.

## Validation

The ROM-free C++ contract executes the bundled image on the ordinary physical
half-clock APU scheduler. It covers dirty low RAM, an unaligned 600-byte block,
a 257-byte second block, another 600-byte block crossing into upper APU RAM,
delayed host polls, counter/page wraps, direct entry, DSP KON, reset and
same/cross-instance restoration during pending instructions and port reads.
Running the same harness with an opaque private original additionally requires
identical readiness, every byte acknowledgment/store and handoff clocks.

```sh
ctest --test-dir build-dmg-firmware -R 'spc700_replacement' --output-on-failure
build-dmg-firmware/gameboy_spc700_replacement_ipl_tests /path/to/private/spc700.rom
```

The eight-byte uploaded DSP fixture reaches readiness at 2,404 SPC clocks and
KON at 2,702, matching the private original. Its synthetic PCM SHA-256 remains
`84d69c40e87dc2258a6476f38de75640a9e09a995e2b348f74d87ca1890ca144`.
The private SGB2 upload diagnostic receives the same five blocks and reaches
the driver with the same instruction counts and host clocks.

The private Donkey Kong full-title gate uses the existing original-override exact WAV
pins, rather than new replacement-specific tolerances. On both SGB models it
replays 60 million host instructions in native, combined, restored and scalar
modes. It checks eleven inputs, three delivered SOUND commands, two audible
commands, more than 5,000 GB frames, exact processor clock ratios, final GB
state parity, and consumer partitioning/restoration. A separate production
adapter test supplies no GB or SPC overrides, retains the existing audio/video
and final-state pins, and exercises 27 whole-host restores per model.

```sh
TMPDIR="$PWD/build-sgb-boot-validation" ctest --test-dir build-dmg-firmware \
  -R 'sgb_replacement_ipl_combined_local|sgb_production_bundled_ipl_local' \
  --output-on-failure
```

These private checks require caller-owned program/game inputs already used by
the existing local suite. Captures stay in ignored build/temporary directories.
They establish software playback parity, not new physical-device listening,
analog hardware equivalence or performance qualification. Further performance
optimization remains deferred; no clocks, rates, gates or PCM pins change.

### Validation run (2026-10-06)

Fresh Linux Release validation passes all four IPL contract/reproducibility,
synthetic PCM and opaque-reference tests. All eight full-title replay modes
pass the unchanged original-override WAV pins, including 586 SGB1 and 538 SGB2
combined-audio restores with exact final GB-state and scalar/chunk parity.
Both production adapter replays pass their existing audio/video/final-state
pins with no GB or SPC override files and 27 whole-host restores per model.
The standard nonlocal/non-performance regression selection passes 165 tests;
three network tests are skipped in the execution environment. These are Linux
software checks, not fresh physical-device or performance qualification.

## Reset, saves and remaining inputs

The bundled image is installed through the same bus API as external firmware.
Whole-host reset retains the configured image. Existing host/APU save formats
are unchanged; snapshots retain actual IPL bytes and the host identity includes
the configured image. States made with a different external IPL require that
same override to load; changing to the bundle does not bypass image identity.

SGB1/SGB2 SNES program ROM replacements remain unimplemented. The default HLE
backend and ordinary GB startup selection remain separate from experimental
desktop firmware playback. Android/web do not gain a firmware playback backend.
