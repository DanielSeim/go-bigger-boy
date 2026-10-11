# Selected score-directory reads

The [controlled field/pitch experiment](sgb-vendor-handoff.md) shows that a
host-port value equal to a music ID is an ambiguous selection marker. This
extension observes native SPC reads of the directory in the authored score
bank, alongside the existing request, host-port, SPC reply and pitch setup
observations. Firmware behavior and production selection remain unchanged.

## Owned controls and observation

The eight existing fixtures retain IDs 1, 2, 3 and their distinct notes, active
or completed playback, and the four effect-STOP profiles. The first three
little-endian directory words occupy `$2B00..$2B05`; their authored roots are
`$2B10`, `$2B14`, `$2B18`. No new proprietary input is used.

`--selection-directory` includes the preceding handoff metadata and adds
`selection_directory_reads`: request number, physical APU half-clock, and
address only. It observes accepted SPC bus reads in the fixed six-byte window
after audible SOUND delivery and before that request's first key-on. It does
not export read values, instruction addresses, RAM snapshots or private bytes.
DSP RAM inspection and echo accesses do not trigger the SPC read binding.

The generic diagnostic binding is excluded from snapshots, retained through
host reset and destination load, and detached on engine destruction. No raw
read callback is installed when diagnostic observation is disabled. Scalar
and batched scheduling must produce the same ordered address/time hash and
exact PCM/state, including a restore during upload.

The checker retains all handoff setup and release contracts, rejects malformed
or unexpected fields, unordered/out-of-window reads and addresses outside the
fixed window, and requires both bytes of the selected word before expected
pitch setup. It retains every read, including other words and repeated visits;
it does not infer validation or playback phases from first/last reads alone.
The additional array has a 128-read cap. Overflow fails. Private captures keep
the 260-million-clock, 240-second and 48-KiB stdout bounds.

## Reproduce

```sh
python3 tests/sgb_vendor_directory_tests.py \
  --timeline-probe build-dmg-firmware/gameboy_sgb_music_timeline_probe
python3 scripts/check_sgb_vendor_directory_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe \
  --firmware-dir roms
```

The optional original comparison executes locally supplied program images
opaquely. Its metadata report records image and fixture identities with
`qualification: false`. Passing these authored workloads does not establish
commercial sound-bank, acoustic, independent-emulator or hardware equivalence.

## Local findings

All 32 private captures (eight fixtures, two models, original/replacement)
pass the expected-note, envelope, request-ownership and final-release checks.
The original reads exactly the selected word, high byte then low byte, once
per request. The replacement reads that word twice, low byte then high byte.
The independently authored `vendor_prepare` source explains its two passes:
whole-score validation followed by live playback. Both passes remain visible;
validation is retained.

Times below are milliseconds at the existing 2048-half-clock/ms scale, over
all three selections and all four STOP profiles in both playback states.

| Program/model | First directory read after SOUND | Last directory read to expected pitch setup |
| --- | ---: | ---: |
| Original SGB1 | 15.053–77.557 | 27.916–50.476 |
| Original SGB2 | 28.295–51.066 | 27.966–30.457 |
| Replacement SGB1 | 0.663–1.287 | 5.598–5.609 |
| Replacement SGB2 | 0.673–1.271 | 5.598–5.609 |

The original byte pair is separated by fourteen half-clocks (6.836 microseconds).
The replacement pair takes 214 half-clocks (104.492 microseconds), with the
second traversal starting 5.147 ms after the first. These differences are
observations of this bounded interpreter workload, not a proposed timer law.

For example, original SGB2's first active baseline selection reads the word
at 28.295/28.302 ms, sets pitch at 58.368 ms, and keys on at 59.412 ms.
Both STOP controls leave the directory times identical while pitch setup
moves by only 0.006 ms. The earlier extra matching host-port write near
33.22 ms therefore occurs after the actual directory lookup. Directory reads
provide a selection-specific boundary, but considerable latency remains on
both sides; they do not establish a universal command-acceptance time.

Next, observe only the fixed authored root, pattern-table and first track
addresses between directory lookup and pitch setup. That should distinguish
pointer traversal from delayed track consumption without private instructions
or data. Repeat the existing STOP controls and then held-out phase controls
before choosing any dispatch pacing. Rapid reselection, STOP/upload ownership
and pinned-title validation still apply to an eventual timing policy.

## Validation evidence

The public suite passes both methods in 232.034 seconds, including sixteen
observed captures and sixteen unchanged ordinary-timeline controls. The
extended native observer contracts pass with exact PCM/state and ordered
read-trace equality across reset, source/destination restore, mid-upload
restore and scalar/batched scheduling. Four dispatch/reply/handoff/directory
report CTests pass; four shard-runner tests pass. CTest discovers the new
matrix exactly once among 91 public matrices with its timeline probe target.

The deterministic DB image remains
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Original program identities remain SGB1
`a75160f7b89b1f0e20fd2f6441bb86285c7378db5035ef6885485eaff6059376`
and SGB2
`c172498a23d1176672931bab33b629c7d28f914a43dca9e540b8af1b37ccf2c6`.
No private program instructions, proprietary score bytes or assets were read
as implementation inputs. Broad earlier firmware matrices, pinned-title,
Windows, independent-emulator and hardware playback were not rerun.
Performance optimization remains deferred.
