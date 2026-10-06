# Controlled vendor song-directory selection

The [address-only observer](sgb-song-selection-observation.md) established
Donkey Kong's code-1 lookup. A new independently authored cartridge separates
directory selection from vendor music, instruments and gameplay input:

```sh
python3 scripts/build_sgb_song_selection_fixture.py --output /tmp/selection.gb
python3 scripts/build_sgb_song_selection_fixture.py --order 3,1,2,2,1,3 --output /tmp/selection-reordered.gb
```

The cartridge is reproducible, ROM-only and SGB-capable. It includes the fixed
cartridge-header verification signature already used by GBB's cartridge parser,
valid header/global checksums and the SGB licensee marker. An initially incomplete
header prevented the original SNES program from releasing the GB clock; those
runs supplied no song-selection evidence. The completed header passes that
startup boundary on both models. This is standard header metadata, without
copied vendor game, music, sample or program content.

Original GB code disables LCD, copies a 4096-byte transport into unsigned VRAM
tiles and maps those tiles over thirteen screen rows. It enables LCD with E4
palette mapping and sends SOU_TRN through actual JOYP pulses. After sixteen
frames it sends SOUND with the first requested music ID; each later ID is
separated by sixteen more frames. Effects and attributes are zero. Supported
fixture orders contain one to eight IDs, each 1, 2 or 3. Reordering and repeats
help distinguish ID selection from request order. CLI errors exit with status 2
without creating partial files; existing outputs are preserved.

The transport has one 32-byte write at `$2B00`, followed by jump `$0400` and zero
padding. Its first three words point to distinct roots `$2B10`, `$2B14` and
`$2B18`, each immediately ending with a zero phrase word. There are no notes,
instruments, sample data, controls or driver instructions in the score bank.
An empty root avoids acoustic prerequisites; it does not test silence of the
whole reference host, which also plays its own startup audio.

## Private reference gate

```sh
python3 scripts/check_sgb_song_selection_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir /path/to/private/reference-images
python3 scripts/check_sgb_song_selection_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir /path/to/private/reference-images --order 3,1,2,2,1,3
```

The directory must contain caller-owned `sgb1.program.rom`, `sgb2.program.rom`
and `spc700.rom`. This optional gate uses the original program and original IPL
as references; production's independent bundled IPL is unaffected. It uses GBB's
independent SGB1/SGB2 GB bootstraps, not vendor GB boot images. No private game
cartridge is needed.

For each model the checker creates a temporary original cartridge and bootstrap,
then runs the legacy fractional APU trace with native GB execution and host/PPU
timing enabled. Each run is bounded to 8000000 SNES instructions and a 180-second
child timeout. It requires the instruction-bound exit and a complete, ordered,
nonoverflowing address report. After directory replacement, each request must
produce exactly the expected high/low pair. Later requests must retain the same
base-write count. Missing, duplicate, reordered or unrelated reads fail the gate.

The checker prints only image hashes, model and request/ID/word-address mappings.
Child stdout/stderr can contain private diagnostics and are never forwarded,
including on errors. Temporary child captures are deleted. A failure exits with
status 2 and no partial JSON. Successful reports retain `qualification: false`
and `playback: false`: the narrowly checked directory contract is not title or
replacement-audio qualification.

## Verified contract, 2026-10-06

Both private original program images passed orders `1,2,3` and `3,1,2,2,1,3`.
The observed lookup is:

| Music ID | Directory word address |
| --- | --- |
| 1 | `$2B00` |
| 2 | `$2B02` |
| 3 | `$2B04` |

For **these three IDs and fixtures**, the word address is
`$2B00 + 2 * (ID - 1)`. Selection follows the requested ID, including repeated
ID 2, rather than the sequence position. Higher IDs, zero/stop codes, malformed
roots, timing of empty-root completion and other bank variants are not covered.
No firmware instructions were inspected, and no proprietary samples or PCM were
exported or added as fixtures. No physical-device or independent emulator
comparison was performed.

Reproducibility pins:

| Input | SHA-256 |
| --- | --- |
| Fixture `1,2,3` | `b098ff796b71599d9e137cf2eab7bef57a80dd6470d4dba1b7fb4b0e2a8d448e` |
| Fixture `3,1,2,2,1,3` | `5f1f7d2a85174c47c248437ea4648ddab125a630eab7f9373d382a6cd0fa6315` |
| GBB SGB1 GB boot | `6238db7b337b4abb2ecf79c5c975951d4dd4fdfb396d71b2f0608b9cc3730903` |
| GBB SGB2 GB boot | `0e4a764a151bc8daf8c06b1348a9ba5c9ad4b5eb61b85fd2ec51009209536add` |

Private program/IPL pins match the preceding observation milestone. ROM-free
tests cover the cartridge/transport checksums, distinct empty roots, strict
orders, failure reporting, metadata-only success output, no-overwrite behavior
and actual two-model JOYP packet sequences. When all three private images exist
under `roms/`, CMake also registers a separate `local;private-reference` gate.
Public CI needs no private images.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(song_selection|score_table_reads)' --output-on-failure
```

This resolves the directory-indexing uncertainty for codes 1..3. The prototype
still rejects vendor score banks. The next renderer contracts remain vendor
pitch/instrument mapping, tempo and articulation, volume/pan behavior and echo;
larger scores also require additional channels and subroutines.

The subsequent [pitch/instrument fixture](sgb-pitch-instrument-fixtures.md)
measures three notes and two resident instruments on logical channel 2, with
matching two-model DSP setup. Broader renderer contracts remain open.
