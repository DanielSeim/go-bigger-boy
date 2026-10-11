# Measured uploaded-instrument voice volumes

This milestone changes the independently authored, unbundled DB music firmware.
It follows the [preceding-tempo transition measurements](sgb-vendor-prior-tempo.md)
with direct original/replacement comparisons and a bounded renderer correction.
It does not qualify the production program ROM replacement.

## Transition observation

The timeline probe's opt-in `--selection-volume` mode records only DSP master
volume `$0C/$1C` and voice-2 volume `$20/$21` writes between an audible SOUND
request and its first KON. It retains the existing directory, traversal and
pitch observations. The array is bounded to 512 entries; overflow fails rather
than truncating. No original instructions, score bytes, tables, samples, RAM
snapshots or PCM are exported. Ordinary timeline output is unchanged.

Four opaque reference captures use the earlier active, base-profile fixtures:
both models, with preceding tempo 45 or 96 and baseline request spacing. SGB2
has no master-volume writes in those selection windows. SGB1 has an upward
startup ramp that continues across song changes: its first selection includes
master values 35..45, its second 59..62 (59..61 at tempo 96), and its third
81..85 (81..86 at tempo 96). These captures do not support introducing a fade-out
on every selection. They do not establish behavior for completed playback,
rapid selection, explicit fade commands or arbitrary startup phases.

Both originals write `6/6` to voice 2 before each note in these fixtures. The
old DB renderer writes `17/17`. The selected score's root delay and the separate
startup master-volume ramp remain unresolved; neither is inferred from this
voice-volume mismatch.

## Firmware correction

`vendor_measured_volume` substitutes only these measured setup points, after
computing the provisional policy and before publishing either voice-volume
register. It applies only to caller-uploaded instrument 2. Resident instruments
and all unlisted operand combinations retain the provisional renderer.

| Pan | Song volume | Track volume | Articulation byte | VOLL | VOLR |
| --- | --- | --- | --- | --- | --- |
| 10 | 160 | 127 | `$7F` | 7 | 7 |
| 0 | 160 | 127 | `$7F` | 0 | 11 |
| 20 | 160 | 127 | `$7F` | 11 | 0 |
| 10 | 80 | 127 | `$7F` | 1 | 1 |
| 10 | 160 | 64 | `$7F` | 1 | 1 |
| 10 | 160 | 127 | `$7D` | 6 | 6 |

The first five points agree with the earlier
[volume/pan fixture observations](sgb-volume-pan-fixtures.md). The new captures
repeat them using the actual owned uploaded instrument and sample binding.
No continuous curve, interpolation, multiplication order or signed-pan rule is
claimed. The full articulation byte is checked, so a different quantization
operand cannot accidentally inherit a velocity-only calibration.

Scratch bytes `$79/$7A` hold computed left/right levels within one synchronous
setup call. Existing emulator RAM serialization includes them; no snapshot
format or host protocol changes are needed. The routine does not wait, add a
fade, change timer targets or change selection ownership. Its additional
instruction cost is not cycle-equivalent to the original.

## Controlled comparisons and lifecycle

`build_sgb_vendor_volume_fixture.py` authors three cases: center/endpoints,
reduced song/track volume, and articulation `$7D`. Each uses the same authored
instrument-2 descriptor `(2,255,224,184,2,176)` and looping BRR bytes. Training
cases use note 24, tempo 45, duration 16 and 16-frame requests. Holdout cases
retain the volume controls but use notes 36/35/34, tempo 96, duration 24 and
19-frame requests. These holdouts test independence from those inputs; they do
not validate additional pan/volume operands. Both originals and the replacement
must produce the exact measured voice pairs, not merely a successful capture.

The supplied tuning word 688 gives pitch 717 in training and pitches
1437/1357/1281 in holdouts. Checks also require all three requests, voice mask 4,
source 2, the owned descriptor's envelope setup, bounded ordered writes before
KON and a completed DSP/echo state. Public tests check an unmeasured song-volume
operand retains the provisional policy and compare observed/ordinary timelines.
Each held-out case on each model runs native and scalar playback with exact
PCM agreement, cold reset, destination save/load and unread-buffer restoration.
These are deterministic owned-audio checks; audible equivalence to proprietary
samples or effective loudness after master volume is not asserted.

```sh
python3 scripts/build_sgb_vendor_volume_fixture.py --case pan --holdout --output /tmp/volume.gb
python3 scripts/check_sgb_vendor_volume_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(vendor_volume_contract|score_vendor_volume)$' --output-on-failure
```

The private checker runs 24 bounded captures (12 original, 12 replacement),
uses no external SPC IPL, suppresses child output on failure and reports only
bounded setup/timing metadata and hashes with `qualification: false`.
Public CI needs no private ROMs. The full public matrix is labelled
`sgb-firmware-extended`; the short fixture/report contract remains in platform
and sanitizer jobs. The optional private gate is labelled `local;private-reference`.

The new deterministic DB image is
`0455c511726ec625afee474a90eba960f5eae2e156b4772d6f7b82b338d36ffd`.
The preceding image was
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Original identities remain SGB1
`a75160f7b89b1f0e20fd2f6441bb86285c7378db5035ef6885485eaff6059376`
and SGB2
`c172498a23d1176672931bab33b629c7d28f914a43dca9e540b8af1b37ccf2c6`.

Next, isolate the tempo-dependent root-consumption wait with additional tempo
and phase controls, then test a bounded scheduling policy against those direct
comparisons. Broader volume/velocity curves, startup fades, echo acoustics,
multi-channel behavior, title qualification and hardware evidence remain open.
Performance optimization remains deferred.

## Local validation evidence

The 24-capture private comparison passes: every original and replacement
voice pair equals the expected measured point, including all holdouts.
The 15-capture public timeline run passes (12 measured cases, two ordinary
observer controls and one unmeasured-operand fallback). Ten focused native/report
CTests pass, as do the four shard-runner contracts. Discovery includes the
new public matrix exactly once among 95 matrices and finds both probe targets.

The full public measured-volume CTest also passes all five methods, including
12 native/scalar replay executions across the six held-out model/case pairs,
with exact PCM, reset, destination load and unread-buffer restoration.

The existing vendor-music suite passes all 12 methods in 588.376 seconds,
covering upload admission, resident/uploaded instruments, scalar/native audio,
STOP, switching, echo and the existing measured gate/completion timing bounds.
The preceding-tempo active/completed reset/save-load replay passes on both
models in 109.706 seconds. The pinned private Donkey Kong playback gate also
passes both models with 637 destination restores each, including 34 unread
buffer restores on SGB1 and 11 on SGB2. These title checks validate bounded
playback and deterministic continuation, not audio equivalence to originals.

Python compilation and whitespace checks pass. Windows, sanitizer runs,
independent emulators, physical hardware and the other broad matrices were
not rerun locally.
