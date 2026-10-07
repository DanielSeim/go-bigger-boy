# Controlled vendor echo-register observations

The [volume/pan fixtures](sgb-volume-pan-fixtures.md) pin selected dry voice
registers. These independently authored notes isolate echo routing, delay and
feedback while keeping instrument, pitch and dry controls fixed.

```sh
python3 scripts/build_sgb_echo_fixture.py --case routing --output /tmp/echo-routing.gb
python3 scripts/build_sgb_echo_fixture.py --case setup --output /tmp/echo-setup.gb
python3 scripts/check_sgb_echo_reference.py --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

Command-width hypotheses come from the repository's limited N-SPC inventory
(F5/F7) and the public [SGB homebrew command notes](https://github.com/cloudsmon/sgb_nspc_music#ebmused-code-list-vcmds)
(F6). That list warns it is not fully tested and marks echo commands as not
implemented in its example. It is a fixture-authoring reference, not evidence
that GBB or the original firmware implements every variant. No firmware
instructions or resident tables were inspected or copied.

Each cartridge uses the shared valid header/checksums, VRAM transport and actual
JOYP code. Its 104-byte bank at `$2B00` contains one phrase and one eight-channel
pattern with channel 2 populated. One SOUND request (ID 1) starts all three notes.
The track sets instrument 2, pan 10, song volume 160, track volume 127 and tempo
96. Before each note it sends F6, waits a duration-64 rest with articulation
`$7F`, sends F7 delay/feedback/filter, waits a duration-16 rest, then sends F5
mask/left/right volume and plays base note 24 for duration 16 with articulation
`$7F`. A duration-16 rest follows each note, and the track ends after note 3.

| Case | Note | Send mask | Echo left/right | Delay | Feedback | Filter |
| --- | --- | --- | --- | --- | --- | --- |
| routing | 1 | 0 | 16 / 24 | 1 | 32 | 0 |
| routing | 2 | 4 | 16 / 24 | 1 | 32 | 0 |
| routing | 3 | 0 | 16 / 24 | 1 | 32 | 0 |
| setup | 1 | 4 | 16 / 24 | 1 | 32 | 0 |
| setup | 2 | 4 | 16 / 24 | 2 | 32 | 0 |
| setup | 3 | 4 | 16 / 24 | 1 | 64 | 0 |

The waits are part of the qualified fixture shape. An initial immediate-setup
probe produced one key-on and then no further DSP writes; its cause was not
established and that probe supplies no successful routing/setup evidence.
The revised fixture completes all three notes. Minimum safe waits, immediate
buffer changes and the full F6 timing contract remain unqualified.

## Measured contract, 2026-10-07

Both private original program images produced identical snapshots with GBB's
independent SGB1/SGB2 GB bootstrap and the private original SPC IPL. All notes
selected physical DSP voice 2, SRCN 2, pitch 1068 and no noise. Echo globals can
still reflect prior state at KON, including a startup EON difference between
models. The observer therefore captures the latest accepted echo-register writes
immediately before each note's first KOF with voice-2 bit set. This checks settled
setup during the note, not the ordering or latency of every intermediate write.

| Case / note | EON | EVOLL / EVOLR | EFB | EDL | ESA | FLG |
| --- | --- | --- | --- | --- | --- | --- |
| routing / 1 | 0 | 16 / 24 | 32 | 1 | `$F7` | 0 |
| routing / 2 | 4 | 16 / 24 | 32 | 1 | `$F7` | 0 |
| routing / 3 | 0 | 16 / 24 | 32 | 1 | `$F7` | 0 |
| setup / 1 | 4 | 16 / 24 | 32 | 1 | `$F7` | 0 |
| setup / 2 | 4 | 16 / 24 | 32 | 2 | `$EF` | 0 |
| setup / 3 | 4 | 16 / 24 | 64 | 1 | `$F7` | 0 |

All six snapshots have FIR register bytes `[127,0,0,0,0,0,0,0]` for filter 0.
Mask 0 clears EON while retaining echo volumes and FLG 0: it does not establish
that echo RAM writes are disabled. Only the two tested delay values and their
ESA allocations are pinned; a general allocation formula is not inferred.
These are DSP register observations, not PCM echo impulse responses, buffer
clearing/writeback validation or sample-equivalence results.

| Independently authored fixture | SHA-256 |
| --- | --- |
| routing | `f6057f4a933cafcd5bf28801046d24b252599896d6aa84ea1c62fd6a1c4db69c` |
| setup | `1cb1f89c71b44c02314dde4d64ab2c77b61908a4512b0e01131855681b62a84e` |

Reference program/IPL/bootstrap pins match the preceding milestones. The checker
prints only image hashes, model, case, note/control metadata and selected DSP
register snapshots. Raw CSVs can contain private RAM diagnostics; those are
ignored, and trace files and captured child stdout/stderr remain temporary and
are never forwarded. No sample or PCM files are exported.

Four runs are each bounded to 8000000 SNES instructions and a 180-second child
timeout. The expected instruction-bound exit is mandatory. The gate requires
exactly three complete KON/KOF pairs, expected voice/source/pitch/noise, every
watched register, exact echo/FIR snapshots and matching two-model results. Missing
or extra notes, retriggers before observation completes, nonpositive gates,
malformed/unordered events, files above 16 MiB and traces at the 32768-row cap
fail. Repeated KOF writes cannot replace the first completed observation.
Failures exit 2 with no partial JSON. Success retains `qualification: false`
and `playback: false`.

ROM-free tests cover the full pattern/command/rest stream, header checksums,
transport boundaries and fixture hash pins; echo writes deferred until after
KON; first-KOF pairing; setup/routing/FIR regressions; malformed/capped traces;
child-output sanitization and CLI no-overwrite behavior. When all three private
firmware files exist under `roms/`, CMake registers an additional
`local;private-reference` matrix gate.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(echo|volume_pan|timing|pitch|song_selection)' --output-on-failure
```

Other masks, signed volume/feedback, filters, delays, echo fades, RAM collision
and clearing behavior, multiple channels, and exact command/write timing remain
open. No physical-device or independent-emulator comparison was performed. The
prototype renderer is unchanged and still rejects vendor banks; commercial-title
playback remains unsupported. These observations constrain future implementation
without supplying proprietary samples or a complete vendor renderer.

The subsequent [two-channel phrase fixtures](sgb-phrase-fixtures.md) measure
combined key-ons and phrase advancement with swapped unequal track durations.
