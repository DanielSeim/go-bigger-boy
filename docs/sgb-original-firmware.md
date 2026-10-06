# Original SNES-side SGB firmware

This is an original **diagnostic playback and transfer prototype**, not a production
SGB program-ROM replacement. It runs an original 65C816 host and an original
SPC700 driver through GBB's existing CPU, ICD, APU and DSP implementation.
There are no firmware interception hooks or host-generated replacement samples.
The generated image is used only by an explicit test/export; desktop selection
and external SGB program-ROM overrides retain their existing behavior.
Performance work remains deferred.

## Reproducible build and provenance

Sources live in [`firmware/sgb/`](../firmware/sgb/): `host.asm`, `driver.asm`,
`samples.asm`, `effects.asm`, and `music.asm`. The dependency-free Python assembler has a strict, fixed-width
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
are newly authored square, stepped-triangle, narrow-pulse and saw waveforms.
Both score motifs are independently composed; no samples or scores are extracted
or recorded assets. Sources/images use GPL-3.0-or-later.

The public [Pan Docs SOUND/SOU_TRN protocol](https://gbdev.io/pandocs/SGB_Command_Sound.html)
defines command framing and parameter layout. GBB's existing ICD and IPL
contracts define register access and upload behavior. This milestone does not
claim cycle equivalence to the proprietary SNES host or acoustic equivalence
to its sound bank.

## Prototype contracts

| Boundary | Required behavior | Prototype |
| --- | --- | --- |
| Cold host entry | Valid LoROM reset vector; disable interrupts; establish register widths | Implemented |
| IPL readiness | Wait for AA/BB before publishing destination, mode and CC token | Implemented |
| SPC upload | Transfer original entries, code, directory and samples starting at $0200; echo every byte index; handle counter/page wraps | Implemented, 5440-byte contiguous payload; resident code at $1000 |
| SPC handoff | Mode zero with a forward token enters $0200; wait for driver readiness | Implemented |
| DSP startup | Driver initializes its voices and master volume before publishing 5A/C4/A5 | Implemented |
| ICD release | Release GB through $6003 with divider 5 after SPC readiness | Implemented on SGB1/SGB2 |
| GB bootstrap packets | Consume the six original GBB GB-side header packets | Implemented |
| ICD packets | Poll $6002; read $7000 exactly once to pop; copy the remaining latched bytes | Implemented |
| SOUND | Support two independent original instruments, explicit start/stop and acknowledgment after DSP writes | Five presets per effect voice plus two original looping scores; controls and restricted codes below |
| Unsupported audio | Retain the offending header and halt packet consumption; stop a compatible driver with a bounded acknowledgment | Implemented; unrecognized drivers are never sent our stop protocol |
| LCD transfers | Capture a full 4096-byte screen through completed ICD planar rows at physical boundaries | Implemented for SOU_TRN, next complete frame |
| SOU_TRN | Validate the complete block list; upload each block; honor the zero-length jump; reacquire compatible drivers | Implemented, including repeated transfers with mailbox v1/v2/v3/v4; transport restrictions below |
| Lifecycle | Restore the actual CPU/WRAM/ICD/APU/DSP/PCM state, including upload; cold reset repeats firmware | Implemented with existing whole-host state codec |

## GBB SPC mailbox versions 1 through 4

The original driver is position independent and explicitly advertises mailbox
version 4 after initializing DSP state. The host retains versions 1, 2 and 3.
The main driver can be relocated; the sample directory/data at `$0500/$0600`
the effect module at `$0640..06AA`, and score module at `$0700..07DF`
remain at their fixed addresses. Uploaded
code/data must preserve them to use this original bank.
After initial startup or an uploaded program's jump, the complete readiness signature is **output ports 0/1/3 =
5A/version/A5**. C1 identifies v1, C2 identifies v2, C3 identifies v3 and C4 identifies v4 and C5 identifies the validated original uploaded-score subset; the host does not infer
compatibility from a jump address, previous ownership, or a partial readiness signature.
Unknown versions and drivers without the signature remain external.

To adopt a recognized driver, the host clears input ports 1, 2 and 3, then
writes input token 0. The driver waits at an arm gate, initializes its last-token value to
zero, and acknowledges with output port 0 = 0. It executes no SOUND command
while waiting for this reset. Only a fresh acknowledged reset grants mailbox
ownership and resets the host counter to zero. This prevents stale IPL inputs
from triggering an effect, and prevents the jump token from aliasing the first
SOUND token (especially a jump token of 1).

Input port 0 subsequently holds the command token; input ports 1/2 hold
instrument A/B. Input port 3 is zero for effect commands. For these commands,
the driver processes each changed token, writes KOF/KON, then echoes that token
on output port 0. The host
waits before sending the next command. Ordinary token rollover FF to 00 is
valid after arming; the reset protocol is used only during adoption.

Input port 3 = 01 with a changed token requests cooperative return to the IPL
loader. A compatible driver must stop its voices, clear its output readiness
and version fields, select direct page zero, enable the IPL overlay and enter `$FFC0`.
The original implementation already runs with direct page zero. The host waits
for AA/BB before any upload. The driver can be uploaded to another RAM address
or restarted in place; adoption still requires the complete signature and reset.
The contract is GBB-specific, not a claim of compatibility with Nintendo N-SPC
or arbitrary third-party drivers.

Arm, SOUND, stop and loader/upload acknowledgments use bounded polling. A
driver that advertises a supported version but fails an operation halts the
host with timeout code 05, without continuing under an unverified mailbox assumption.

Version 1 uses one token per SOUND and accepts attributes zero only. Version 2
first stages attributes with input port 3 = 02, attributes on input port 1 and
input port 2 = 00. The driver stores them and echoes the token without applying
them to DSP sound state. After that acknowledgment, the host sends A/B codes with port 3
= 00 and a second token. The driver applies staged attributes and effects before
acknowledging. This separates attributes (which can themselves contain 01/02)
from loader control, and lets save/load retain a partially staged command.

Version 3 adds effect presets and scores. It stages attributes in input port 1
and the score code in input port 2 using control 02, then commits both with the
A/B effect phase. Pending music cannot take effect during staging. Control 03
silences all three voices before an unsupported command halts the host; v1/v2
retain their two-effect stop command and reject v3 presets/scores before staging.

## Original SOUND instruments and controls

Command header `41` accepts A/B values `00`, `01..05`, `80` with v4.
Version 3 remains limited to `00`, `01..03`, `80`.
The original effect bank uses voice 6 for A and voice 5 for B:

| Preset | A waveform | B waveform |
| --- | --- | --- |
| 01 | Square | Stepped triangle |
| 02 | Narrow pulse | Narrow pulse |
| 03 | Saw | Saw |
| 04 (v4) | Rising saw | Triangle with vibrato |
| 05 (v4) | Falling triangle | Square with tremolo |

These are authored diagnostic presets, not reproductions of the proprietary
SGB effect names/assets at those IDs. The presets use authored looping BRR samples and, for 04/05,
timer-driven modulation. A applies its decaying gain and B sustains. Code 00 retriggers the remembered
preset, and 80 stops it and clears that memory. Dummy code 00 does nothing if
there is no remembered preset; it can retrigger A after its envelope reaches
zero. V1/v2 remain limited to codes 00/01/80 and music zero; v1 accepts only
zero attributes and keeps its previous driver-defined zero-code behavior.

Music byte 4 accepts 00 (keep current score), 01/02 (restart one of two newly
composed eight-note motifs), and 80 (stop music). Voice 4 plays the triangle
sample at fixed volume 30 and direct gain 50. The separate score interpreter
advances notes every 16 timer ticks (256 ms at the default SPC clock), loops
after eight notes, and keeps its countdown/cursor in SPC RAM. Pitch units are
explicitly authored data in `music.asm`; this is not an N-SPC score decoder.
Effect commands do not advance the score clock. Effect and music stops are
independent; the existing master-volume fade applies to all three voices.
DSP levels and command codes here are hexadecimal; tick counts/times are decimal.

| Attribute field | v2/v3/v4 behavior |
| --- | --- |
| Bits 0..1 | A pitch: DSP 0800, 0C00, 1000, 1800, from low to high |
| Bits 2..3 = 0/1/2 | A voice volume: 40, 28, 10; also request fade-in |
| Bits 2..3 = 3 | Global mute/fade-out; retain A's previous voice volume |
| Bits 4..5 | Independent B pitch, same four DSP steps |
| Bits 6..7 = 0/1/2 | B voice volume: 40, 28, 10 |
| Bits 6..7 = 3 | Reserved/unsupported; reject the packet |

Timer 0 supplies physical SPC ticks: prescale 128, target 128, so 16 ms at the
default 1.024 MHz APU clock. A starts at direct gain 60 and loses four units each
tick, reaching silence on its 24th decay tick (about 368..384 ms, depending on the
free-running timer phase). B sustains direct gain 60 until stopped. Global master
volume moves between 60 and zero by eight units per tick; a full fade takes
12 ticks (about 176..192 ms, also phase-dependent). Fade direction can reverse
before it completes. DSP register values in the table and gain/master-volume
value 60 are hexadecimal; prescales, tick counts and elapsed times are decimal. These are newly authored sound parameters,
not measurements or copied presets from a proprietary SGB sound bank.

Version 4 advances A04/A05 pitch by one high-byte unit per timer tick, starting
at the selected A attribute pitch and clamping to `$0100..3F00`. Both retain the
24-tick decay; retrigger resets the pitch cursor to the selected base. B04
alternates pitch between the selected base plus/minus two high-byte units.
B05 alternates direct gain 60/20. Each B phase lasts two physical ticks (32 ms
at the default SPC clock, for a 64-ms period). Retrigger resets the B phase;
changing back to a steady B preset restores direct gain 60. Stop cancels the
modulation, and silence-all also clears the remembered effects. All values
are authored diagnostic parameters, not measurements of proprietary effects.
Version 4 adds no mailbox fields; its version advertises the expanded code range.
Version 3 rejects new presets before staging, while retaining its score controls.

All parameter/envelope/fade work runs in the SPC firmware and writes the real
DSP registers. There is no host gain ramp or generated replacement PCM. At
other configured APU clock rates, durations follow the hardware timer clock.
Broader effect coverage and sound-bank compatibility remain pending. Other
effect/score IDs, malformed SOUND/SOU_TRN framing,
reserved B volume, and nonzero attributes sent to a v1 driver halt explicitly.

Other commands are consumed by the prototype; GB-side visual commands continue
through the existing SGB display adapter. SNES menus, borders, general WRAM
transfers/JUMP execution, controller negotiation and a hardware SNES PPU are
outside this diagnostic milestone. Such commands are not validated by this
prototype and must be audited before production integration.

## Authoring and uploading score data

The v3/v4 score table is sixteen pitch high bytes at `$07D0..07DF`: eight notes
for score 01 followed by eight for score 02. Each note retains the interpreter's
16-tick duration; the selected motif loops on the music voice. The original
[`score_example.json`](../firmware/sgb/score_example.json) contains two authored
variations with different opening pitches from the bundled motifs. The samples,
interpreter and timing remain those of the original diagnostic firmware.

```sh
python3 scripts/build_sgb_score_transfer.py firmware/sgb/score_example.json --output /tmp/gbb-sgb-scores.bin
```

The JSON schema is exactly `{"scores": [[eight integers], [eight integers]]}`.
Pitch units must be integers 1..63, representing DSP pitches `$0100..3F00`;
these are hardware pitch units, not MIDI note numbers. Booleans, floating-point
values, missing/extra fields, duplicate keys and malformed JSON are rejected.
There is no rest or tempo field in this format. Validation completes before an
output file is created, and the tool refuses to overwrite an existing file.

The output is a raw 4096-byte **VRAM transfer payload**, not a ROM: a 16-byte
block for `$07D0`, a zero-length jump to `$0200`, then zero padding. A GB program
must prepare its unsigned tiles/map and identity palette while LCD is off as
described below, enable LCD, send SOU_TRN `49`, and allow capture/upload/rearming
to finish before selecting score 01 or 02 through SOUND `41` byte 4. Restarting
the driver clears active effects/music; a subsequent SOUND explicitly starts
playback. The updated score table survives this restart, while a host cold reset
reloads the bundled table before the GB program repeats its transfer.

`--driver-entry 0x0800` can target an **already installed** relocated compatible
driver. The tool checks transport address bounds and excludes the score table;
it does not install a driver or establish compatibility. The host still requires
the complete readiness signature and arm acknowledgment. Keep the fixed sample
and interpreter addresses intact. This is an authoring path for the original
GBB v3/v4 bank, not a Nintendo N-SPC score decoder or proprietary title-data format.

## Screen capture, upload and ownership

A valid `49` SOU_TRN packet starts capture. The host waits for VBlank and the
following frame, then waits for each completed eight-line ICD row. Thirteen
fixed-source `$7800` to `$2180` DMAs store twelve 320-byte rows and a final
256-byte row at WRAM `$1000..1FFF`. This reconstructs the first 256 tiles,
including the partial last visible tile row. Selection/readout stays ahead of
ring-bank reuse. Missed row boundaries and LCD-off waits fail explicitly;
there is no direct GB VRAM read or host-side screen synthesis. The cartridge
must prepare the normal unsigned, consecutive-tile, unscrolled, identity-palette
transfer display before sending SOU_TRN.

The 65C816 validates the **entire** captured block list before stopping the
resident driver or writing any transferred data. Each four-byte header must fit,
all source data must fit within 4096 bytes, destinations must not wrap past
64 KiB, and a zero-length jump must terminate the list. The transport reserves
`$0000..00FF` for IPL scratch and I/O, so write destinations must be at least
`$0100`. Jump addresses must be `$0100..FFBF`, excluding I/O and the active IPL
overlay. This is stricter than the generic `SgbSoundTransfer` syntax parser.
Writes beneath the IPL overlay remain allowed; an uploaded program can disable
the overlay to read that physical RAM. A block ending exactly at `$FFFF` is
validated and tested. Bytes after the terminating jump are unused.

On successful validation, host port 3 requests transfer ownership. The compatible
SPC driver keys off all voices, clears its output signatures, enables the IPL
overlay and jumps to `$FFC0`. The host waits for AA/BB, uploads each block through
normal IPL counter/acknowledgment handshakes, then sends the mode-zero jump.
Block-transition tokens advance by at least two and are forced odd so an echoed
command cannot alias the first data-byte index zero.
Loader readiness and each acknowledgment have bounded polling loops. Validation
failure keeps the resident driver in place and sends its ordinary stop command;
a loader/acknowledgment failure halts with an error without pretending that the
resident mailbox still exists.

The jump initially hands APU ownership to the uploaded program. During polling,
the host observes output ports 2/3 and checks for the complete supported readiness
advertisement. It adopts only after a successful arm handshake. Compatible
uploaded drivers then accept the restricted SOUND subset and further SOU_TRN
requests. Tests upload the original driver to `$0800` and `$0C00`, then restart
the resident driver through its `$0200` entry, with SOUND start/stop after each handoff.

Unrecognized drivers remain external. A subsequent SOUND or SOU_TRN request
halts packet consumption without sending commands to them. Their audio can
continue; unknown drivers cannot safely be silenced through our former mailbox.
A jump to the former resident address alone does not confer ownership, because
transferred data could have overwritten its code. Unsupported SPC opcodes
retain the existing bounded-host fault behavior. General N-SPC/third-party
interoperability and title sound-bank compatibility remain pending.

Diagnostic WRAM bytes:

| Address | Meaning |
| --- | --- |
| `$20` | 00 startup, 01 resident driver, 02 uploaded driver, 03 capture/validation, 04 IPL upload, 05 driver arm, 06 stop, FF halted |
| `$21` | Offending audio command header |
| `$22`, `$23` | Packet count modulo 256; resident mailbox token |
| `$24` | Zero while a verified mailbox is owned; one after release |
| `$25`, `$26` | Completed captures; acknowledged uploads/jumps, modulo 256 |
| `$27` | 01 malformed/range error, 03 transport-reserved address, 04 LCD timeout/missed row, 05 driver-arm/command/loader/upload timeout |
| `$28`, `$29` | Last observed external-program output ports 2/3 |
| `$2A` | Completed supported-version adoptions modulo 256, including cold startup |
| `$2B` | Version from the most recent adoption attempt (C1/C2/C3/C4) |
| `$40..43`, `$44..45` | Current block size/destination; validated jump address |
| `$2C` | Current effect-code upper bound, exclusive (02 v1/v2, 04 v3, 06 v4) |
| `$0100..010F` | Last packet |
| `$1000..1FFF` | Exact captured 4096-byte payload |

Halt keeps GB/DSP clocks running and stops processing new packets. It is a
diagnostic state, not a playable fallback; further packet production can
ultimately overflow the bounded ICD queue.

## Validation and remaining gates

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_original_firmware_tests gameboy_sgb_original_transfer_tests
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_original_' --output-on-failure
```

The ROM-free executable uses real JOYP transactions from an original homebrew
cartridge. It checks both models, the bundled GB boot/IPL, sound output and
stopping, native and combined audio, scalar/optimized agreement, snapshots
during execution, cross-instance continuation, cold reset and unsupported
audio. The transfer contract additionally checks all 4096 latch bytes, VBlank
updates after command delivery, multiple uploaded blocks, counter/page rollover,
upper RAM through `$FFFF`, execution and audio from an uploaded SPC diagnostic,
invalid source/destination/header/jump cases, bounded LCD-off failure, ownership
changes, and cross-instance restoration during capture/upload/after handoff.
It also checks repeated transfers, actual relocated driver execution, resident
restart, SOUND after every jump, ordinary token rollover, exact scalar PCM/state,
reset replay, and cross-instance restore during the arm handshake. Independent
SPC stubs check unknown versions, missing arm/SOUND acknowledgments, and a
claimed v1 driver that never returns to IPL. SOUND checks measure independent
A/B pitch and amplitude ordering, A decay/retrigger, sustained B and mute/unmute
fades in real PCM. Native/combined scalar parity, reset and cross-instance restore
cover modulation and an attribute stage pending its effect command. Legacy v1
command dispatch succeeds without staging and rejects new attributes explicitly.
V2 stubs accept attribute staging but reject v3 presets/scores; v3 stubs accept
their original bank/score controls and reject v4 presets before staging. The original
bank checks distinct rendered presets, remembered preset retriggers, timer-driven
score changes, score-only output, concurrent effects/music, independent stops,
global music mute, and silence after unsupported commands. Dynamic effect
checks measure rising/falling A pitch, decay to silence, B vibrato frequency
changes, tremolo amplitude changes, cancellation on preset replacement, and
native/combined scalar parity, reset and restoration of in-flight modulation. The score-authoring
contract generates an actual payload with the CLI, then verifies exact ICD
capture, data-only upload/rearming, both score selections, measured pitch ratios
against the bundled motifs, native/combined scalar parity, reset and active-score
restore on both models. It also rejects malformed authoring data and preserves
existing output files. Native/combined scalar parity, cross-instance continuation
and reset also cover the built-in active scores.
These tests establish an original functioning pipeline, not title
compatibility or parity with the proprietary program ROMs.

Validation on 2026-10-06: all five prototype CTest checks passed in the Release
build. The preceding core regression run, before the score-authoring test was added,
(`-E 'local|performance|gameboy_sgb_original_(firmware|transfer)'`)
reported 169 tests, zero failures and three skipped network tests. Export checks
confirmed the exact 262144-byte image, matching generated SHA-256, and refusal
to overwrite an existing image. Private-title, physical-device and performance
qualification were not run for this prototype.

Before bundling a program default, define general uploaded-driver and sound-bank
interoperability, expand the original effect/music bank and remaining command semantics,
cover controller/display interactions, and validate real SGB1/SGB2
titles with original-vs-replacement **behavioral** gates. Different original
sound assets cannot satisfy proprietary waveform hashes; retain those existing
hashes as regression gates for external-image playback and define separate
reviewable expectations for the new firmware. Preserve explicit program-ROM
overrides and state-image identity checks when integrating a default.

## Title demand and next compatibility priority

The [resident score contract](sgb-resident-score-contract.md) provides the next
layout/restart design and an offline data-subset oracle using original fixtures.
Its acceptance does not establish SPC playback or mailbox ownership.
The driver now resides at `$1000`, with a legacy `$0200` trampoline. The `$0400` entry validates the explicit
[GBS1 subset](sgb-uploaded-score-v5.md) before advertising v5; other data remains
silent and external with E1/E2 diagnostics. Vendor title-score rendering remains
unsupported.

A [title-demand audit and bounded native probe](sgb-original-title-demand.md)
cover local Donkey Kong, Kirby's Dream Land 2 and Tetris Attack runs on both
models. Their recorded SOUND operands fit the current subset, but Donkey Kong
uploads 1619 bytes to `$2B00`, jumps to `$0400`, and never adopts a compatible
driver; its following SOU_TRN halts. This prioritizes a resident uploaded-score
restart/format contract before further diagnostic presets. The audit and probe
are inventory tools, with explicit post-transfer evidence limits and no title
qualification claim.
