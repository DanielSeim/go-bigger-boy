# Audible one-shot interruption and restart

The [natural-completion gate](sgb-one-shot-audio.md) now extends to SOUND stop
while an owned transient is still audible, followed by a fresh SOUND restart.
A looping SNES voice and an uninterrupted GB pulse accompany both onsets.
Actual key-off, terminal ENDX, envelope and stereo PCM observations distinguish
an interrupted sample from one that had already ended before the command.

This is bounded diagnostic evidence. General replacement qualification and
production playback remain false; production requires private SGB1/SGB2 program
ROMs. No firmware, DSP, mixer, emulator-core or save-state-format changes are
needed. Hardware, independent DSP arithmetic, original-bank timbre and title
compatibility remain unqualified. Performance work remains deferred.

## Owned fixture and command timing

`build_sgb_one_shot_interrupt_fixture.py` reuses the preceding owned four-block,
filter-zero/range-eleven transient and calibrated two-block square/triangle loop.
The same 2048-byte score and 192-byte sample-object bounds, normal tuning,
instrument map, right-only SNES routing, volumes and left-only GB pulse apply.
The held voice uses authored note 33; its peer uses note 24. Both use admitted
duration 64/articulation 127 at tempo 96. Each physical voice takes the held role
in turn. The lower transient note provides a longer audible interval in which
to interrupt; natural playback remains bounded to four BRR blocks.

Four GB stage markers accompany SOUND start, stop, restart and final stop.
The GB pulse starts only at stage 1 and remains routed left through stages 2
and 3; the cartridge mutes its routing at stage 4. Delays are 64, 2, 4 and 32
GB frames, with an additional 1800-iteration GB spin before the first stop.
That fixed instruction sequence runs on the actual emulated GB CPU; the probe
never injects commands or changes RAM. Native onset and key-off observations,
rather than an assumed packet-delivery latency, anchor acoustic windows.

`build_cartridge` now accepts optional bounded `spin_delays`, one integer
0..2499 per command, after its ordinary VBlank waits. A nonzero count emits
`LD BC,n; DEC BC; LD A,B; OR C; JR NZ`, consuming `28*n+8` GB clocks.
The largest count stays below one 70224-clock GB frame. Defaults emit no extra
instructions and retain previous ROM bytes. This timing resolution is needed
because a whole-frame-only stop trial reached native KOF after terminal ENDX.
The fixed fixture delay was changed to hit active playback; acoustic acceptance
limits were not relaxed to accept that late stop.

The `both`, `voice2`, `voice3` and `gb` source controls retain identical score,
cartridge code, lengths, descriptors, directory, headers and scheduling.
Inactive sources have only BRR data nibbles zeroed. All voices still execute
KON, KOF and native envelopes; controls are not shifted or aligned afterward.
Exports refuse overwrite and carry reproducible header/global checksums.

## Native, PCM and replay contracts

The separate `gameboy_sgb_one_shot_interrupt_probe` captures 55 million master
clocks at 48 kHz. Exactly four notes are required: two physical voices before
stop and two after restart. Note entries retain the preceding nine onset/
release/zero fields and add ENVX and masked ENDX at KOF. Natural completion
entries contain ENDX and envelope-zero half clocks and actual output frames.
Only the second transient may have a natural-completion entry. The first must
have positive ENVX and clear ENDX at KOF; retired notes cannot acquire a later
retrigger's ENDX. New observations require ENDX to clear and the envelope to
become active after their own KON.

The checker requires the first onset before the authored stop marker and its
KOF within 960 output frames after that marker. Both voices must have positive
observed ENVX when interrupted. The restarted one-shot must reach natural
ENDX/zero within 768 frames of KON, at most 32 frames apart, before gate release.
The held loop must never be classified as a one-shot completion.

Each execution repeats with exact in-place save/load and cold reset. Full host
state, PCM, envelope observer and all note/action/completion observations must
match. Saves occur before draining at markers, envelope events, KOF/zero,
natural completion and periodic checkpoints. Additional queued-output saves
cover the 32 output frames after every native KON and KOF. All four onset bits
and all four key-off bits must appear in these unread-output coverage masks,
including actual interruption and restarted playback. Marker, release/zero and
natural-completion coverage masks are checked separately. A scalar `both` run
must match the complete batched PCM and reported observations.

`check_sgb_one_shot_interrupt.py` also requires:

- Identical action/note/completion timelines and source sample counts across all
  four controls, and byte-identical full left GB streams.
- Full-run residual `both - voice2 - voice3 + gb` within the unchanged four-unit
  fixed-point rounding budget in both channels, with zero clipping and exact
  silence in the GB-only control's right channel.
- At least sixteen nonzero frames in the interrupted transient and 32 in fresh
  playback, peaks of at least 100 units, and bounded onset/duration. At least
  eight of the 32 frames immediately before the first KOF must be nonzero with
  peak at least 100. Positive ENVX alone cannot satisfy audible interruption.
- Last audible output more than sixteen frames earlier relative to KON than
  in the naturally ending restarted sample. Both SNES voices must settle to exact
  silence throughout a stop gap of at least 2048 frames while GB remains active.
- Exact transient silence and equality of the full right mix with the audible
  looping control after fresh natural completion. Held pitch across the second
  transient and GB pulse frequency during the stop gap retain the preceding
  2048-frame detector and 10-cent limits. Final stereo tails must be exactly zero.

Each child retains a 90-second deadline, 1024-restore cap, 600000 PCM-byte bound
and 8192-byte metadata bound. Temporary PCM and states are not committed; reports
retain hashes, counters, event times, transient peaks/durations and pitch results.
These controls share the current renderer and establish internal consistency,
not independent DSP arithmetic or original-firmware acoustic equivalence.
No private image, instruction, score, sample or PCM is an implementation input
or committed artifact. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_one_shot_interrupt_probe
python3 scripts/check_sgb_one_shot_interrupt.py \
  --probe build-dmg-firmware/gameboy_sgb_one_shot_interrupt_probe
python3 scripts/build_sgb_one_shot_interrupt_fixture.py \
  --held-voice 3 --profile both --output /tmp/one-shot-interruption.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'gameboy_sgb_one_shot_interrupt'
```

The matrix covers both models and both held roles, with four source profiles and
one scalar `both` control per group: twenty runs, each with reset and restored
executions. The full-matrix CTest has a 2400-second timeout. Public guards reject
late stops, false active/ENDX/completion data, missing queued-output coverage,
inaudible interruption, absent restart, stale tails, held dropout, GB disturbance,
shifted timelines and incorrect source sums. Subframe guards exercise bounds,
default identity, instruction encoding, payload preservation, deterministic
builds, checksums and exporter refusal.

## Validation evidence

All twenty acoustic runs pass: sixteen batched source controls and four scalar
`both` controls across both models and held roles. Each emits 122920 stereo
frames, with zero clipping and exact whole-state/PCM/observer equality after
cold reset and in-place save/load. Every run performs 438..441 restores,
including 168 with unread output. All four marker, release, zero, queued-KON
and queued-KOF bits are present. Natural-completion masks are 8 for held voice 2
and 4 for held voice 3, identifying only the restarted transient.

The first native KOF occurs 75..77 output frames after its authored stop marker,
148..203 frames after KON. Interrupted transient ENVX is 62..127 and terminal
ENDX remains clear. Isolated interrupted PCM has 139..191 nonzero frames and
peaks of 210..612 units. Its last audible frame is 150..204 frames after KON;
every run passes the immediately-before-KOF audibility guard. Fresh playback
has 211..212 nonzero frames, peaks of 256..612 units and last audible output
223..224 frames after KON, passing the unchanged shortening margin in all four
groups. Thus a native active-envelope observation is backed by actual truncated
PCM and fresh audible restart, rather than a late command after completion.

All four stop gaps are exactly silent on the SNES/right channel while the GB
pulse continues. These gaps span 4515..4579 frames. After fresh natural
completion, all four source-isolation windows contain exact transient silence
and equality of full right PCM with the audible looping control for
15396..15401 frames. Final stereo tails are exactly zero. Full left GB streams
and action/note/completion timelines match byte-for-byte across all controls.
Maximum source residual is zero units left and one right, below the unchanged
four-unit budget. All scalar controls match complete PCM and observations.

Four held-pitch windows have maximum absolute error 2.311743 cents, at least
sixteen periods and maximum period deviation 0.230474 frames. Four GB-pulse
measurements during stopped SNES playback have maximum error 0.023060 cents.
Both remain below the preceding 10-cent limit.

Five public guard methods pass. Ten focused CTests pass in 50.89 seconds,
covering the new guards, preceding one-shot/polyphony/transition/combined/native
and isolated pitch guards, firmware build/reproducibility and DSP envelopes.
The new probe builds and both CTests are registered. Before/after comparison
shows all 110 preceding atomic fixture variants remain byte-identical with
default delays; all preceding one-shot completion fixture hashes also match.
Python compilation, new-file whitespace checks and `git diff --check` pass.
The full twenty-run matrix ran directly through the registered checker and was
not redundantly rerun through CTest.

No new private-original, hardware or independent-DSP comparison was performed.
Prior full octave/mapping, one-shot block-count, broad title and other acoustic
matrices were not rerun. The new evidence covers this fixed owned four-block
transient, normal map/tuning, admitted fixed pan/volume/gate points, default
modeled clocks and 48-kHz output. Other source lengths, command phases, clock
settings and presentation rates remain separate qualification work.

## Next step

The [instrument-change gate](sgb-instrument-change-audio.md) covers looping to
one-shot and back on one voice while its peer and GB audio continue, including
reset/save-load and scalar parity. Next, qualify audible replacement of an
owned uploaded bank with fresh samples and instrument mappings. Broader banks
and independent hardware/DSP comparisons remain separate qualification work.
