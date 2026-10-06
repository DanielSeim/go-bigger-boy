# Original SNES-side SGB firmware

This is the first original **diagnostic playback prototype**, not a production
SGB program-ROM replacement. It runs an original 65C816 host and an original
SPC700 driver through GBB's existing CPU, ICD, APU and DSP implementation.
There are no firmware interception hooks or host-generated replacement samples.
The generated image is used only by an explicit test/export; desktop selection
and external SGB program-ROM overrides retain their existing behavior.
Performance work remains deferred.

## Reproducible build and provenance

Sources live in [`firmware/sgb/`](../firmware/sgb/): `host.asm`, `driver.asm`, and
`samples.asm`. The dependency-free Python assembler has a strict, fixed-width
instruction subset, resolves branches, rejects range errors and overlapping
sections, and writes a deterministic 256 KiB LoROM image with header/checksum.
The checked-in `prototype_image.hpp` holds the host, payload and ROM header;
it is included by the test executable, not the production core.

```sh
python3 scripts/build_sgb_prototype.py
python3 scripts/build_sgb_prototype.py --check
python3 scripts/build_sgb_prototype.py --check --output /tmp/gbb-sgb-prototype.rom
```

The export refuses to overwrite a file. For explicit diagnostic playback it
can be supplied as `SgbHostConfig.program_rom`. The same image serves both
models; select the matching `HardwareModel` and GB bootstrap normally.
No user-owned firmware or sound bank is a build/test input. The BRR fixtures
are newly authored square and stepped-triangle waveforms, not extracted or
recorded assets. Sources/images use GPL-3.0-or-later.

The public [Pan Docs SOUND/SOU_TRN protocol](https://gbdev.io/pandocs/SGB_Command_Sound.html)
defines command framing and parameter layout. GBB's existing ICD and IPL
contracts define register access and upload behavior. This milestone does not
claim cycle equivalence to the proprietary SNES host or acoustic equivalence
to its sound bank.

## First milestone contracts

| Boundary | Required behavior | Prototype |
| --- | --- | --- |
| Cold host entry | Valid LoROM reset vector; disable interrupts; establish register widths | Implemented |
| IPL readiness | Wait for AA/BB before publishing destination, mode and CC token | Implemented |
| SPC upload | Transfer original code, directory and samples to $0200; echo every byte index; handle counter/page wraps | Implemented, 1042-byte contiguous payload |
| SPC handoff | Mode zero with a forward token enters $0200; wait for driver readiness | Implemented |
| DSP startup | Driver initializes its voices and master volume before publishing A5/5A | Implemented |
| ICD release | Release GB through $6003 with divider 5 after SPC readiness | Implemented on SGB1/SGB2 |
| GB bootstrap packets | Consume the six original GBB GB-side header packets | Implemented |
| ICD packets | Poll $6002; read $7000 exactly once to pop; copy the remaining latched bytes | Implemented |
| SOUND | Support two independent original instruments, explicit start/stop and acknowledgment after DSP writes | Restricted diagnostic subset below |
| Unsupported audio | Silence voices and retain the offending command header; halt packet consumption | Implemented |
| LCD transfers | Capture full 4096-byte transfer screens through completed ICD planar rows at physical boundaries | Next milestone; existing ICD row contracts already tested separately |
| SOU_TRN | Parse length/destination packets; upload all data; honor the zero-length jump; handle driver ownership changes | Next milestone; deliberately rejected here |
| Lifecycle | Restore the actual CPU/WRAM/ICD/APU/DSP/PCM state, including upload; cold reset repeats firmware | Implemented with existing whole-host state codec |

The private host/SPC command protocol is token on port 0, instrument A on
port 1, instrument B on port 2. The SPC echoes port 0 only after writing KOF/KON.
The SNES host waits for that echo before submitting another command. SPC port 3
publishes A5 readiness; host port 3 is reserved after upload. The two-port boot
readiness signature is distinct from the IPL signature and cannot be mistaken
for an upload token echo.

Only command header `41`, attributes `00`, music `00`, and instrument values
`00` (leave this voice), `01` (trigger its new looping tone), and `80` (stop)
are accepted. A uses voice 6, B voice 5. They have fixed tuning/level and
sustain until stopped; original effect indexing, A's decrescendo, pitch/volume,
mute/fade, music and retrigger timing are not yet implemented. Other SOUND
codes, malformed SOUND framing, nonzero attributes/music and any SOU_TRN
request cause the unsupported path. This keeps unknown audio observable
instead of substituting a generic tone for every original effect.

Other commands are consumed by the prototype; GB-side visual commands continue
through the existing SGB display adapter. SNES menus, borders, general WRAM
transfers/JUMP execution, controller negotiation and a hardware SNES PPU are
outside this diagnostic milestone. Such commands are not validated by this
prototype and must be audited before production integration.

Diagnostic WRAM bytes: `$20` = 00 during startup, 01 running, FF unsupported
halt; `$21` = offending audio command header; `$22` = packets copied modulo
256; `$23` = host/SPC command token. `$0100..010F` retain the last packet.
Unsupported halt keeps the GB and DSP clocks running, but stops processing
new packets; this is a diagnostic state, not a playable fallback. Further
packet production can eventually overflow the bounded ICD queue.

## Validation and remaining gates

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_original_firmware_tests
ctest --test-dir build-dmg-firmware -R gameboy_sgb_original_firmware --output-on-failure
```

The ROM-free executable uses real JOYP transactions from an original homebrew
cartridge. It checks both models, the bundled GB boot/IPL, sound output and
stopping, native and combined audio, scalar/optimized agreement, snapshots
during execution, cross-instance continuation, cold reset and unsupported
audio. These tests establish an original functioning pipeline, not title
compatibility or parity with the proprietary program ROMs.

Validation on 2026-10-06: all three prototype CTest checks passed in the Release
build. The existing regression run (`-E 'local|performance|gameboy_sgb_original_firmware'`)
reported 169 tests, zero failures and three skipped network tests. Export checks
confirmed the exact 262144-byte image, matching generated SHA-256, and refusal
to overwrite an existing image. Private-title, physical-device and performance
qualification were not run for this prototype.

Before bundling a program default, implement complete transfer capture and
SOU_TRN driver handoff, define the original sound/music bank and command
semantics, cover controller/display interactions, and validate real SGB1/SGB2
titles with original-vs-replacement **behavioral** gates. Different original
sound assets cannot satisfy proprietary waveform hashes; retain those existing
hashes as regression gates for external-image playback and define separate
reviewable expectations for the new firmware. Preserve explicit program-ROM
overrides and state-image identity checks when integrating a default.
