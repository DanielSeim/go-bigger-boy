# Audible stopped-bank replacement

This gate extends [voice-local instrument changes](sgb-instrument-change-audio.md)
to a physical SOU_TRN replacement of the uploaded score and sample bank. An
owned square loop is stopped, both bounded RAM regions are cleared, and a fresh
bank reverses the instrument map and selects an owned triangle loop at a new
pitch. GB audio continues through stop, upload, publication and restart.

This is bounded owned diagnostic evidence. General replacement qualification
and production playback remain false; production still requires private
SGB1/SGB2 program ROMs. Firmware, DSP, mixer, emulator core and state format are
unchanged. Independent DSP arithmetic, physical hardware, original-bank timbre
and broad title compatibility remain unqualified. Performance work stays deferred.

## Owned banks and controls

`build_sgb_bank_replace_audio_fixture.py` uploads two 2048-byte scores and
192-byte sample objects, each in its own bounded 4096-byte physical payload.
Both banks retain the preceding calibrated 32-sample filter-zero/range-eleven
loop geometry, normal tuning and admitted envelope/pan/volume points. Exports
are deterministic, checksummed and refuse overwrite.

The first bank maps IDs 2/10 to physical sources 2/3. Its active voice selects
ID 2/source 2, an owned square cycle, at authored note 33 (440 Hz). The second
bank reverses the map to IDs 10/2 and replaces the selected source-3 waveform
with the independently authored triangle cycle. ID 2 then selects source 3 at
note 36 (523.251 Hz). Both notes use tempo 96, duration 64/articulation 127,
song/voice volume 160/127 and right-only routing. The other physical voice rests.
Each voice takes the active role on each model.

Five actual GB command stages use VBlank delays 64, 12, 4, 64 and 12: SOUND
start, SOUND stop, replacement SOU_TRN, SOUND restart, final SOUND stop. The
replacement stage copies its payload through GB VRAM before issuing SOU_TRN;
the cartridge builder's existing copy and two-frame LCD settling sequence apply.
The GB pulse starts left-routed at stage 1 and remains active through stage 4.
Only stage 5 mutes its routing. HRAM markers precede each real command; native
KON/KOF and output frames anchor acoustic windows rather than assumed command
latency. No probe writes commands, RAM or DSP registers.

Four profiles preserve identical cartridge code, score bytes, directories,
descriptors, maps, BRR headers, block counts and scheduling. Only BRR data
nibbles are silenced:

| Profile | Initial bank data | Replacement bank data |
| --- | --- | --- |
| `both` | owned | owned |
| `old` | owned | zeroed |
| `new` | zeroed | owned |
| `gb` | zeroed | zeroed |

All native onsets and envelopes still execute. Controls are never shifted,
rescaled or fitted to captured output.

## Native transaction and replay evidence

The separate `gameboy_sgb_bank_replace_audio_probe` captures 100 million master
clocks at 48 kHz. It observes exactly two notes on the selected physical voice,
first from source 2 and then source 3. Each must have stable setup, a real KOF,
positive ENVX at KOF and envelope zero afterward. Retired notes stop being
sampled once their own release reaches zero, before a new upload's DSP epoch;
they cannot accumulate later DMA sampling gaps or retire a newly started note.

The shared read-only envelope observer accepts an optional upload generation,
defaulting to the preceding generation 1. This probe observes generations 1
and 2. Existing probes retain their default behavior.

At each native A4 clear publication, the probe verifies every byte of
`$2B00..32FF` and `$5000..50BF` is zero, and score/root readiness is invalidated.
At A5 publication of each uploaded generation, it records score and asset FNV
hashes and both map bytes. The checker matches these to the independently built
owned fixture bytes; a remapped SRCN alone cannot stand in for changed samples.
Clear and publication events must occur in the corresponding initial/replacement
intervals and before SOUND playback.

Every execution repeats with exact in-place save/load and cold reset. Full host
state, stereo PCM, envelope observer, command stages, notes, clears and bank
publications must match. Saves happen before draining at periodic checkpoints,
markers, envelope events, KOF/zero and native clear/publication events.
Additional saves with unread output cover 32 frames after every KON, KOF,
clear and publication. All two-note, two-clear and two-publication coverage bits
and all five marker bits are required. Scalar `both` runs must match complete
batched PCM and reported observations.

Each child has a 150-second deadline, 1536-restore cap, 1000000-byte PCM bound
and 16384-byte metadata bound. Temporary states and PCM are not committed.

## Acoustic acceptance

`check_sgb_bank_replace_audio.py` requires identical native timelines and sample
counts across all four profiles. It also requires:

- Byte-identical whole left GB streams and exact silence in the GB control's
  right channel. The model-specific GB pulse pitch is measured during the
  replacement clear/upload interval.
- Whole-run stereo residual `both - old - new + gb` within the preceding
  four-unit rounding budget and zero clipping.
- An active old loop when its stop command takes effect, followed by envelope
  zero before upload. Both bank KOF observations occur within 960 frames of
  their own stop marker, with positive ENVX and loop ENDX observed.
- At least 4096 frames of exact right-channel silence between old release and
  new onset. The old-only control remains exactly silent for the rest of the
  capture; the new-only control is silent before its own onset.
- Exact equality of full right output to the old control through its lifetime,
  then to the new control from restart onward. Both early and late windows of
  each note must have the authored pitch using the existing 2048-frame detector
  and 10-cent limit. The two measured window hashes must differ. Fresh bank
  byte hashes, SRCN, map, pitch and actual PCM are checked together.
- Exactly zero final stereo tails after the last stop settles.

These same-renderer controls establish internal consistency, not independent
DSP arithmetic or original-firmware acoustic equivalence. No proprietary image,
instruction, score, sample or PCM is an implementation input or committed asset.
The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_replace_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_replace_audio_fixture.py \
  --voice 3 --profile both --output /tmp/bank-replacement.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_replace_audio_contract$'
```

The full matrix has twenty runs: both models and both voices, each with four
source profiles plus scalar `both`. Each run includes restored/reset executions.
CTest registers it with a 3600-second timeout and `sgb-firmware-extended` label
for dedicated Linux matrices. Public guards stay in platform and sanitizer jobs.

## Validation evidence

All twenty matrix cases pass across both models and physical voices. Each emits
223492 stereo frames with zero clipping and exact whole-state/PCM/observer
equality after cold reset and in-place save/load. Each performs 781..782
restores, including 309..311 with unread output. All five marker bits and both
note release/zero, queued-KON/KOF, clear/publication and queued-clear/publication
bits are covered. All four scalar cases match complete batched observations
and PCM. Native event timelines match exactly across source controls.

Every clear has zeroed score/sample RAM and invalidated readiness; every bank
publication matches the corresponding owned score/sample hashes and map.
Both notes are active at KOF, with ENVX 96..97; KOF follows its stop marker by
73..90 output frames. Replacement intervals contain 60474..61870 frames of
exact right-channel silence. Old-only output never returns, new-only output
starts only in its own lifetime, and final stereo tails are exactly zero.

Whole left GB streams match byte-for-byte. Maximum source residual is zero
units on both channels, below the unchanged four-unit budget. Sixteen early/late
bank pitch measurements have maximum absolute error 2.820511 cents, at least
seventeen periods and maximum period deviation 0.249630 frames. The four GB
upload-window measurements have maximum error 0.119677 cents. Every pitch
result is below the existing 10-cent limit.

The five public guard methods pass, including owned sample-data-only controls,
explicit right-only score panning, deterministic export and overwrite refusal,
both models/voices, malformed native transaction/replay data and acoustic
mutations. Rejection cases cover missing fresh audio, late dropout, wrong pitch,
stale old output, output during clearing, GB disturbance, timeline differences
and source-sum errors. The final registered contract passes in 9.97 seconds.

Twelve focused CTests pass, covering the new contract and preceding instrument
changes, one-shot interruption/completion, polyphony, transitions,
combined/host/isolated pitch, shard runner and firmware build/reproducibility.
The unchanged default envelope-observer behavior also reproduces the preceding
SGB1 voice-2 instrument-change run's complete metadata and PCM SHA-256
`46c5eeb6354c940b4a0bdd37bae60891fcbf86bb20c93531a3ba08207936dafc`,
including its reset and restored executions.

CTest assigns this full matrix to exactly one of eight dedicated Linux shards;
the extended set now contains 66 tests. The public contract stays unlabelled.
Probe build, Python compilation, shard planning and whitespace checks pass.
The full checker used native results from this session, retaining temporary
per-case results while executing independent voice assignments concurrently.
Cached fixture hashes were verified before reuse; all four source comparisons
and scalar parity ran through the checker. The matrix was not redundantly
rerun through CTest.

No new private-original, physical-hardware or independent-DSP comparison was
performed. Earlier complete acoustic and broad title matrices were not rerun.
Evidence covers these two fixed owned banks, normal tuning, admitted
envelope/pan/volume points, default modeled clocks and 48-kHz output. Other
banks, command phases, clocks and presentation rates remain separate work.

## Next step

Qualify replacement SOU_TRN while the old owned loop is still audible, without
a preceding SOUND stop. Anchor interruption to native KOF/mute and clearing,
then verify fresh remapped playback, uninterrupted GB audio and queued-output
replay across the active ownership transition. General banks, private-original
or hardware comparisons and title compatibility remain separate work.
