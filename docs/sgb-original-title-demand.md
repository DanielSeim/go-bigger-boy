# Original SGB title-demand inventory

The next compatibility priority is an independently written resident score
interpreter and restart contract for uploads to `$2B00` followed by a `$0400`
jump. The measured Donkey Kong run fails at that boundary before it can use the
prototype's existing score controls. Adding more diagnostic effect IDs alone
would not resolve that failure.

This is command demand and failure triage on 2026-10-06, not title qualification,
visual/audio equivalence, or physical-device validation. The original prototype
remains opt-in and production playback still requires external program ROMs.
No proprietary firmware, score bytes, samples, raw packets or full traces are
included in this document or added as fixtures.

## Tools and scope

`report_sgb_original_compatibility.py` reads existing bounded GBB JOYP traces,
checks SOUND framing and parameter limits for mailbox v1..v4, inventories
unexecuted SNES host commands, and marks the first SOU_TRN as an evidence
boundary. JOYP contains neither the transferred screen nor the SPC readiness
signature. Later SOUND operands can be in the supported parameter range while
the uploaded driver remains incompatible; the report never calls that working
playback. Its JSON includes counts, IDs and evidence labels, not raw packets.

```sh
python3 scripts/report_sgb_original_compatibility.py /tmp/title.trace
python3 scripts/report_sgb_original_compatibility.py /tmp/title.trace --mailbox-version 4 --json
```

The separate `gameboy_sgb_original_title_probe` uses the original program image,
original bundled GB bootstrap and original bundled SPC IPL, then executes the
real host/GB/SPC/DSP. It drains PCM without saving it and stops at a prototype
halt, host-step failure or requested master-clock boundary. It opens only the
caller-supplied game; initial battery RAM and game inputs are not supplied, and
it never writes a save. Its JSON includes diagnostic counters and captured
transfer block addresses/sizes, but no block data or snapshots.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_original_title_probe
build-dmg-firmware/gameboy_sgb_original_title_probe sgb2 /path/to/title.gb 90000000
```

The default clock bound is 120000000; explicit bounds must be 1..200000000.
Stepping stops at instruction boundaries, so the final instruction may exceed
the requested clock target. A successfully emitted report has exit status 0
for **every** outcome, including halt/failure; `qualification` is always false.
Invalid inputs have exit status 2. A clock-limit result means only that no
terminal diagnostic was reached in that window.

## Captured requests

HLE JOYP captures used `gbb_test_runner`, both SGB models, fresh cartridge RAM,
and these windows. They inventory the title's requests rather than replaying
native program firmware. Native probes below use separate cold, unseeded runs
with no input script; the two paths are not frame-aligned parity comparisons.

| Title | HLE frames / input | Commands per model | SOUND codes / scores | SOU_TRN |
| --- | --- | --- | --- | --- |
| Donkey Kong (JU) v1.1 | 3600 / existing Donkey Kong gameplay script | 45 | A/B: 00,80; score: 00,01 | 2, first at sequence 10 |
| Kirby's Dream Land 2 (U) | 600 / none | 34 | A: 00; B: 01,80; score: 00 | 0 |
| Tetris Attack (U) v1.0 | 600 / none | 18 | No SOUND request in this window | 0 |

Both models produced the same demand counts for each title. None of these
captured SOUND operands needs an additional effect/score ID beyond v4. That
does not validate the authored assets used for those IDs. All three titles send
eight DATA_SND commands and one PAL_PRI; Donkey Kong additionally sends two
ICON_EN commands. Those SNES host behaviors are not executed by the prototype
and need separate behavioral review.

Input SHA-256 pins:

| Input | SHA-256 |
| --- | --- |
| Donkey Kong game | `b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4` |
| Kirby game | `08ddc36709d551b6c2b768e8280e0213ebe89a5088b34e1cc6c0e14977b8e312` |
| Tetris Attack game | `b8765e752153310f835e10d6ceaee80b0e0913cdadf832b9516af8bf974e9666` |
| Donkey Kong input script | `a5d37081cc52b8bfe72284f5cd836b0ccea9bfc3ddf25fcc1ac2769b3b2e802b` |
| Original program image | `a633abaaf88ff69842df27dbdf02102f416b9ad43f1eee3f4ab26f894bcd740c` |

## Native prototype outcomes

| Title / model | Clock bound | Outcome / observed frames |
| --- | --- | --- |
| Donkey Kong / SGB1 | 90000000 | Halt at 68508252 clocks, frame 140 |
| Donkey Kong / SGB2 | 90000000 | Halt at 70139894 clocks, frame 140 |
| Kirby / SGB1 | 200000000 | Clock limit at 200000010, frame 435, two SOUND packets delivered |
| Kirby / SGB2 | 200000000 | Clock limit at 200000004, frame 421, two SOUND packets delivered |
| Tetris Attack / SGB1 | 90000000 | Clock limit at 90000016, frame 218 |
| Tetris Attack / SGB2 | 90000000 | Clock limit at 90000016, frame 212 |

For both Donkey Kong models the captured list is valid: one 1619-byte block to
`$2B00`, then jump `$0400`. One capture/upload/handoff completes; no transfer
error is reported. Driver adoptions remain at one (cold startup), ownership is
external, and the next `49` SOU_TRN halts with diagnostic state FF and unsupported
header 49. This is an observed mailbox/restart incompatibility, not a failed
screen capture or a missing SOUND operand.

The public [Pan Docs sound contract](https://gbdev.io/pandocs/SGB_Command_Sound.html)
identifies `$2B00..4AFF` as score memory and `$0400` as the original N-SPC restart
entry. Together with the transfer metadata, that supports the **inference** that
this title expects a resident score engine at `$0400`, rather than installing a
complete replacement driver in this upload. The GBB v4 driver lives at `$0200`
and has a different sixteen-byte motif format at `$07D0`; its program also
occupies `$0400`. Redirecting that jump or acknowledging the score code would
not supply the required decoder or original sound-bank behavior.

Kirby and Tetris reaching their clock bounds does not qualify their startup,
visuals, assets or audio. These probes compare no proprietary PCM or independent
hardware/reference output and do not cover later gameplay. The next firmware
milestone should define a reviewable resident restart/layout and uploaded score
format contract, with original fixtures, before attempting title score playback.

## Automated checks

```sh
ctest --test-dir build-dmg-firmware -R gameboy_sgb_original_compatibility_report --output-on-failure
```

The ROM-free checks exercise version-specific parameter ranges, reserved
attributes, framing and command order, the post-transfer evidence boundary,
metadata-only JSON, and the actual probe using original homebrew GB code. They
verify a bounded unsupported-SOUND halt on both models, clock-limit/input errors,
and real score-data capture/upload/rearming with only block metadata reported.
The title runs above use local private inputs and are not automatic pass gates.
