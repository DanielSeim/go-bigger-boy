# Observing vendor song selection

The candidate roots in the [vendor demand inventory](sgb-vendor-score-demand.md)
do not establish how SOUND selects a song. The native reference trace helper
now accepts `--score-table-read-output PATH` to observe reads of `$2B00..2B05`,
the first three candidate directory words, without inspecting firmware code or
exporting score values.

```sh
cmake --build build-dmg-firmware --target gameboy_snes_65c816_apu_trace
build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  /path/to/sgb2.program.rom /path/to/spc700.rom \
  --sync-gb-sgb2 /path/to/game.gb /path/to/sgb2.boot.rom \
  --fractional-apu-sync --native-gb-input \
  --input-script tests/fixtures/sgb/titles/donkey-kong-gameplay.script \
  --ppu-dma-timing --host-bus-timing --instruction-limit 45000000 \
  --score-table-read-output /tmp/song-table-reads.json
```

This diagnostic uses caller-owned original images and the legacy fractional APU
trace path. It is unavailable with `--core-apu-engine`; the production scheduler
is not changed. The output directory must exist and the path must be unused and
distinct from other configured outputs. Existing files are preserved. A normal
instruction-bound result emits the report and exits with status 4, matching the
existing trace helper; input errors exit with status 2. Earlier terminal faults
can end without a report. Neither a report nor an exit code qualifies playback.

The JSON contains total read/write counts in the six-byte window and the first
256 reads. Each stored event contains a master clock, SPC half clock, address,
number of nonzero SOUND packets delivered, and number of prior writes to the
base address `$2B00`. Data bytes, instruction PCs, opcodes, score events and PCM
are excluded. A base write count helps distinguish observations before and after
replacement, but does not prove an entire upload completed or identify its bank.

After 256 reads the collector keeps total counts, sets `overflow: true`, and
discards later events without stopping CPU execution. The helper's instruction
bound limits the execution window. The fixed address window is explicit; the
collector does not discover a song table elsewhere in RAM.

These are physical bus reads, including dummy reads before SPC stores. Startup
uploads can produce reads of all six addresses while writing the directory.
Such reads cannot establish song selection. The `audible_packets` field counts
nonzero SOUND requests, including effects: it does not identify a music code.
Correlate a later pair of directory reads with separately observed host music
commands and completed uploads before inferring a selection rule. Reads alone
do not establish the phrase pointer value, sound-bank identity, DSP behavior,
or correctness of the replacement's future mailbox.

ROM-free tests execute independently authored SPC MOV/store/branch fixtures
through the real bus observer. They check dummy reads, prior-write ordering,
address filtering, overflow without early termination, exclusion of data fields,
CLI output and existing-file protection. The original GBS playback grammar and
vendor upload rejection stay unchanged.

## Private reference evidence, 2026-10-06

Fresh runs used the private original SGB1/SGB2 program images, original GB boots
and original SPC IPL, the Donkey Kong v1.1 input pinned in the
[demand audit](sgb-original-title-demand.md), and the existing gameplay script.
The script applies Start at frame 1000, A at 1600, and later movement/jump inputs.
Each run used a 45000000-instruction bound, default 1024000 Hz APU clock,
fractional synchronization, native input and host/PPU timing flags shown above.
No firmware instructions were inspected, and no PCM file was exported.

| Model | Final master clocks | GB frames | SOUND delivered / nonzero | SOU_TRN |
| --- | --- | --- | --- | --- |
| SGB1 | 1595564384 | 3895 | 3 / 2 | 2 |
| SGB2 | 1597324950 | 3881 | 3 / 2 | 2 |

Both reached the instruction bound with exit 4, not a CPU fault. The first
nonzero SOUND request occurred at GB frame 2472 with music code 1 and zero
effect/attribute fields. Separately captured host-port observations confirmed
music value 1 was sent to port 0. After that delivery, the SPC read `$2B01` and
then `$2B00`. The second nonzero request produced the same address pair.
These occurred after two prior writes to the base address; no further base
writes occurred before the end of either run.

| Model | First post-delivery `$2B01` read | Following `$2B00` read |
| --- | --- | --- |
| SGB1 | 1056746374 master clocks | 1056746520 |
| SGB2 | 1050254760 master clocks | 1050254912 |

Each address summary contained 18 reads and 12 writes in the six-byte window,
two base writes and no overflow. Earlier reads accompanied boot/title uploads
or the firmware's startup music; they are excluded from the selection inference.
Cold idle runs and runs with only a Start press produced no nonzero SOUND
request and therefore supplied no transferred-bank selection evidence.

This supports a **limited black-box contract**: in these runs, music code 1
selects the first directory word at `$2B00`, rather than the word at `$2B02`.
Combining this with the previously captured bank supports candidate root
`$2B06` for that request. The pointer value is inferred from the separately
captured upload, not emitted by the address observer. A general one-based rule
for codes 2 and 3 remains unvalidated. Stop/restart semantics, empty entries,
out-of-range codes, other bank variants, instrument mapping, tempo, articulation,
echo and replacement PCM behavior also remain unqualified.

Reference image SHA-256 pins:

| Image | SHA-256 |
| --- | --- |
| SGB1 program | `a75160f7b89b1f0e20fd2f6441bb86285c7378db5035ef6885485eaff6059376` |
| SGB2 program | `c172498a23d1176672931bab33b629c7d28f914a43dca9e540b8af1b37ccf2c6` |
| SPC IPL | `c95f88b299030d5afa55b1031e2b5ef2dff650c4b4e6bb6f8b1359436521278f` |
| SGB1 GB boot | `0e4ddff32fc9d1eeaae812a157dd246459b00c9e14f2f61751f661f32361e360` |
| SGB2 GB boot | `fd243c4fb27008986316ce3df29e9cfbcdc0cd52704970555a8bb76edbec3988` |

These private runs are not CI pass gates or physical-device validation. Native
reference execution and the original diagnostic prototype are separate paths;
the prototype's commercial score rejection is still open. No proprietary image,
score, sample, raw packet or private trace is committed.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_score_table_reads|gameboy_snes_spc_(bus_cycle|half_cycle|write_cycle)' --output-on-failure
```
