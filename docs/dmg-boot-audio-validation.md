# Cartridge audio after DMG replacement boot

The replacement remains experimental and opt-in. This is a **same-core**
comparison of cartridge sound after two different CPU-executed boots, not an
independent hardware audio reference. Nintendo firmware is an opaque local
input; no instructions, graphics or sound assets are incorporated into GBB.

Current DMG firmware is revision 4, which preserves revision 3's APU alignment
and changes idle serial initialization; see
[serial validation](dmg-boot-serial-validation.md). The findings and full-suite
totals below are historical revision-3 captures. The revision-4 record reports
unchanged replacement PCM hashes and passing audio gates, not sample equality
to original boot. No private audio comparison was rerun for this audit.
Generic startup IDs, version-42 ordinary snapshots and bundled model scope are
summarized in [boot validation](dmg-boot-validation.md#current-contract-and-historical-evidence-audited-2026-10-05)
and [firmware](../firmware/README.md). SGB firmware-host pins and performance
are separate [host evidence](sgb-host.md#current-evidence-status-audited-2026-10-05).

## Local comparison

Build the test-only probe in an optimized configuration:

```sh
cmake -S . -B build-boot -DCMAKE_BUILD_TYPE=Release -DGAMEBOY_BUILD_SDL=OFF
cmake --build build-boot --target gameboy_dmg_boot_probe
python3 scripts/compare_dmg_boot_audio.py \
  --probe build-boot/gbb_dmg_boot_probe \
  --reference-boot /path/to/your/dmg_boot.bin \
  --rom /path/to/your/title.gb \
  --seconds 30 --press-start-second 4 \
  --output /tmp/dmg-title-audio.json
```

Repeat `--rom` to compare more cartridges; all receive the same input schedule.
Repeat `--press-start-second` for multiple Start pulses, or omit it for an
untouched intro/title screen. Each pulse lasts 70,224 T-cycles (one GB frame),
starts relative to cartridge handoff, and is applied at the next instruction
boundary. Actual press/release cycles are recorded for both boots. Captures
begin immediately after handoff and exclude boot sound. They cover a fixed
instruction-cycle budget, with at most one instruction's overshoot; they are
not stretched, shifted, frame-aligned or volume-normalized to conceal differences.

The tool uses Python's standard library only. It refuses to overwrite reports.
PCM is 48 kHz, little-endian signed 16-bit stereo. Length and FNV hash must match
the probe metadata, and the report includes SHA-256 capture/input/probe hashes.
Input hashes are checked again after execution. Adjacent battery saves are not
loaded and the original cold-reset baseline is unchanged (no four-clock offset).

By default private PCM captures are temporary and removed after comparison.
For listening/debugging, add `--capture-directory /tmp/new-private-captures`:
this must be a new directory. Each boot/title gets PCM, a stereo WAV and a
provenance manifest. These contain cartridge audio and are **private local
artifacts: do not commit them, include them in releases, or publish them as CI
artifacts**. No raw ROM/VRAM snapshots or wave RAM are saved in that bundle.
Reports contain metrics and hashes, not PCM or wave RAM contents.

Exit codes: `0` measured gates pass; `1` mismatch; `2` invalid input/tool/capture
failure; `3` insufficient audible coverage. Two silent captures are not success.

## Gates and limits

The sound-register command sequence must match exactly, including frequency,
envelope, panning, power and wave RAM writes. The private trace is discarded;
its values do not enter the report. Matching commands verify programmed pitch
and driver behavior, but cannot alone verify channel clocks or actual output.

The additional PCM gates use independent stereo channels, DC-centered RMS and
Hann-windowed spectra (2,048-point FFT; 40 logarithmic energy bands). Anti-phase
stereo is not folded into mono and mistaken for silence. The same 50 ms windows
are compared without alignment search. Separate absolute/per-channel gain gates
prevent a normalized spectral comparison from hiding quieter output or panning
errors. Thresholds are fixed in the tool and copied into each report:

| Gate | Limit |
| --- | --- |
| Sound command timing difference | 1 ms maximum |
| First active-window onset difference | 50 ms |
| Active duration / active-window disagreement | 100 ms |
| Longest internal silent-gap difference | 50 ms |
| Overall and per-channel level difference | 0.5 dB |
| 95th-percentile active-window level difference | 2 dB |
| Mean spectral-band cosine similarity | At least 0.98 |
| Mean relative spectral-centroid difference | At most 3% |
| Additional clipped samples | At most 0.01 percentage point |
| Audible coverage | At least 0.5 seconds in each capture |

An active window has centered RMS >= 64 int16 counts (about -54 dBFS).
Clipping counts samples with magnitude >= 32,760, and reports the original's
absolute clipping too: clipping in both paths is not described as clean output.
Spectral centroid is not a per-voice pitch estimate for polyphonic music.
Shorter-than-window defects or very quiet effects can escape these gates.
These are regression checks, not proof of perceptual or sample-exact equality;
listening and independent hardware/reference captures remain useful.

## Historical revision 3 findings (2026-10-04)

The first 30-second check with Start at four seconds failed Pokémon Blue:
commands matched, but inherited APU state produced excessive window-level and
spectral differences. Its driver writes `NR52=80` without powering the APU off,
so the previous boot's frame sequencer and pulse phase remain relevant.

Opaque reference execution exposed sequencer step 1, silent CH1 duty step 2,
timer 30 and envelope zero at handoff. Its subsequent timer reload reveals a
252-clock pulse period: the probe's **actual** 104 followup clocks change timer
30 to 178 after one reload (`30 - 104 + 252 = 178`). No boot instructions were
read to obtain this observation.

GBB's replacement firmware now establishes those inherited states
using ordinary writes and counted delays: period `7C1`, two normal muted
triggers, natural envelope decay, a 352-clock waveform delay and four forced
DIV-APU falling edges. It does not inject private state or reset/mute the host
audio engine. Visible registers, DIV=`ABC8`, LCD line 153 dot 396 and checksum
flags retain their contracts. Startup is about 1.031 emulated seconds. The
resampler phase still differs, so raw PCM hashes are not claimed equivalent.

The register/trigger behavior follows the public
[Pan Docs audio registers](https://gbdev.io/pandocs/Audio_Registers.html) and
[DIV-APU/pulse clock description](https://gbdev.io/pandocs/Audio_details.html).
The deliberately omitted original boot animation/chime is not recreated.

All four titles pass the unchanged gates after this correction:

| Title | Sound-command timing delta (max) | Overall level delta | Mean spectral similarity |
| --- | ---: | ---: | ---: |
| Pokémon Blue (UE) | 0.621 ms | 0.00019 dB | 0.99994 |
| Super Mario Land v1.1 | 0.0105 ms | 0.00004 dB | 0.99995 |
| Tetris v1.0 | 0 ms | 0.00003 dB | 0.99997 |
| Donkey Kong (JU) v1.1 | 0.520 ms | 0.00495 dB | 0.99795 |

Onset, active duration and longest-gap deltas are zero at the 50 ms measurement
resolution in all four checks. Pokémon's 95th-percentile window-level delta
fell from 2.040 dB to 0.014 dB; the overall delta fell from 0.0722 dB to
0.00019 dB. The no-input Super Mario Land title screen was largely silent:
Start input supplies meaningful music coverage instead of treating silence as
a passing audio comparison. The checks use no initial battery saves.

Completed-frame comparisons after 60 million followup cycles still match for
all four titles. This is short deterministic startup/title coverage, not full
gameplay or physical-speaker validation. The original boot's four-clock reset
discrepancy remains open; see [boot validation](dmg-boot-validation.md).

## Offline regressions

```sh
ctest --test-dir build-boot -R 'gameboy_dmg_' --output-on-failure
```

The new test entry uses original logo-free homebrew and generated GBB firmware,
never external Nintendo inputs. It exercises CPU-written pulse notes, stereo
panning, length-limited effects, an original wave table, noise envelopes and
quiet gaps. A driver that leaves the APU powered on is checked against known
512/1,024 Hz notes and length/panning behavior. Other tests inject detuning,
gain changes, dropped audio, delayed onset, clipping, stereo imbalance and
command-timing errors, and require each corresponding gate to fail. Silent or
truncated captures cannot pass, PCM tampering is detected, files are not
overwritten, and enabling capture/trace does not alter emulated state or PCM.

Historical revision-3 validation on 2026-10-04: the full optimized suite completed with 159
passing tests and three network-related skips. All seven focused firmware/PPU
checks and all four focused ASan/UBSan checks passed (LeakSanitizer disabled
for the sandbox). The four-title audio comparison was rerun with the final
analysis tool and passed. Existing SGB title and PCM regression baselines also
passed in the full suite. These results do not establish real-hardware audio
equivalence.
