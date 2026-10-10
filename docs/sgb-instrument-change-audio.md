# Audible loop/one-shot/loop instrument changes

This gate extends [one-shot interruption and restart](sgb-one-shot-interruption.md)
to voice-local instrument changes inside a real uploaded score. One physical
SNES voice selects a loop, a naturally completing one-shot, then the loop again.
Its peer sustains the same loop throughout, while a left-routed GB pulse continues.
Both SGB models and both physical assignments are exercised.

This remains bounded owned diagnostic evidence. General replacement
qualification and production playback remain false; production still requires
private SGB1/SGB2 program ROMs. Firmware, DSP, mixer, emulator core and save-state
format are unchanged. Independent DSP arithmetic, physical hardware,
original-bank timbre and broad title compatibility remain unqualified.
Performance optimization remains deferred.

## Fixture and controls

`build_sgb_instrument_change_audio_fixture.py` reuses the preceding 192-byte
owned bank: a calibrated two-block square or triangle loop and a four-block
descending bipolar transient. BRR uses filter zero/range eleven; the transient
ends with B1 (END without loop), and the loop ends with B3. Padding, directory
entries, normal tuning, envelope descriptors and mapped IDs 2/10 retain the
preceding admitted geometry. The score remains bounded to 2048 bytes and the
complete physical SOU_TRN payload to 4096 bytes, including its handoff entry.

The held voice plays authored note 33 with duration 64/articulation 127. Its
peer plays note 36 three times, selecting instruments with executed E0 prefixes:
loop, one-shot, loop. The first two gates use duration 16/articulation 63; the
returned loop uses duration 16/articulation 127. Tempo 96 and song/voice volumes
160/127 are unchanged. Only existing admitted gate points are used; the
firmware's bounded timing table is unchanged.

Both voices share the loop source. Four sample-data-only profiles preserve
identical scores, cartridge code, descriptors, directories, headers, lengths
and scheduling:

| Profile | Loop data | One-shot data |
| --- | --- | --- |
| `both` | owned | owned |
| `loop` | owned | zeroed |
| `transient` | zeroed | owned |
| `gb` | zeroed | zeroed |

Only BRR data nibbles are zeroed in inactive sources; all native notes and
envelopes still execute. Both SNES voices remain right-routed, with identical
admitted pan and volume points. Source controls avoid the parser and DSP latch
phase differences that arise when changing pan to isolate physical voices.
The shared loop control contains both frequencies during the first and returned
loop, and only the held frequency while its peer selects the transient.

All cartridges run identical GB code. They start the pulse and mark stage 1
before SOUND start, then mute GB routing and mark stage 2 before final SOUND
stop. There are 64 GB VBlank waits before each command. The probe observes
actual native KON/KOF and output frames; it never injects commands or RAM
changes, shifts controls, fits gains or reconstructs the measured output.
Exports are deterministic, checksummed and refuse overwrite.

## Native, replay and acoustic contracts

The separate `gameboy_sgb_instrument_change_audio_probe` captures 55 million
master clocks at 48 kHz. Its read-only observer requires exactly four native
notes: one held note and three peer selections. SRCN and pitch must follow the
authored sequence. Every note must release and reach zero without setup changes
or missed native samples. Only the one-shot source is eligible for terminal
completion; looping ENDX is not mistaken for a naturally ending transient.
New completion observations require cleared ENDX and an active envelope after
their own KON. Retired notes cannot acquire a later selection's ENDX.

Each execution repeats with exact in-place save/load and cold reset. Full host
state, stereo PCM, envelope observer and all action/note/completion observations
must match. Saves occur before draining at markers, periodic checkpoints,
envelope events, releases, envelope zero and natural completion. Additional
queued-output saves cover the 32 frames after each native KON/KOF. All four
onset, release, zero and queued-onset/key-off bits must be covered, including
both instrument changes. ENDX and natural-zero restore masks identify only the
transient. One scalar `both` control per group must match complete batched PCM
and observations.

`check_sgb_instrument_change_audio.py` requires:

- Identical action, native note, natural-completion and sample-count timelines
  across all four profiles, and identical whole left GB streams. No timeline
  tolerances, gain fitting, control alignment or PCM shifts are used.
- Whole-run stereo residual `both - loop - transient + gb` within the unchanged
  four-unit fixed-point rounding budget, zero clipping, and exactly zero right
  output in the GB-only control.
- A one-shot with at least 32 nonzero frames, peak at least 100, first output
  within 128 frames, and last output 32..768 frames after KON. Terminal ENDX
  and natural envelope zero precede gate release, occur within 768 frames of
  KON, and are at most 32 frames apart.
- Exact transient silence from natural zero plus 32 frames until four frames
  before the returned loop, for at least 512 frames. The full right mix equals
  the continuing loop control throughout that interval.
- Exact switching-voice silence in both release gaps, with audible held output
  and equality of the full mix to the loop control. Release and zero must
  precede the next source's KON, in both native clocks and output frames.
- The existing 2048-frame zero-crossing pitch detector and 10-cent limit for
  the isolated held loop while its peer selects the transient, and for the
  model-specific GB pulse. Final stereo tails are zero.
- Both 440-Hz held and 523.251-Hz peer fundamentals in the shared loop control
  during the first and returned loop, including a late returned-loop window.
  A fixed 2048-frame Hann periodogram removes weighted DC and scans each target's
  +/-20-Hz band at 0.5-Hz spacing using Goertzel power. Each peak must lie inside
  its scan band, be within 10 cents of the target and have fundamental amplitude
  at least 100 PCM units. This rejects missing or wrong-pitched components and
  a brief attack followed by silence. It measures the existing output; it does
  not reconstruct, rescale or subtract an inferred waveform. These owned tones
  and their separation define the diagnostic's scope, not a general-purpose
  polyphonic pitch detector.

Each child has a 90-second deadline, a 1024-restore cap, a 600000-byte PCM bound
and an 8192-byte metadata bound. Temporary samples and states are not committed.
Reports contain hashes, counters, observed events and acoustic measurements.
These same-renderer controls establish internal consistency, not independent
DSP arithmetic or original-firmware acoustic equivalence. No proprietary image,
instruction, score, sample or PCM is an implementation input or committed asset.
The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

## Reproduce

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_instrument_change_audio_probe
python3 scripts/check_sgb_instrument_change_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_instrument_change_audio_probe
python3 scripts/build_sgb_instrument_change_audio_fixture.py \
  --held-voice 3 --profile both --output /tmp/instrument-change.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_instrument_change_audio_contract$'
```

The full matrix has 20 runs: two models times two held assignments times four
source profiles plus a scalar `both` control. Each run includes reset/restored
executions. CTest registers the matrix with a 2400-second timeout and the
`sgb-firmware-extended` label, retaining dedicated Linux full-matrix CI coverage.
The shorter public contract remains in platform and sanitizer jobs.

## Validation evidence

All twenty runs pass: sixteen source controls and four scalar `both` controls
across both models and held assignments. Each emits 122920 stereo frames with
zero clipping and exact whole-state/PCM/observer equality after cold reset and
in-place save/load. Every run performs 531..534 restores, including 249 with
unread output. All four note release/zero and queued-KON/KOF bits are covered;
both stage bits are covered. Natural-completion masks are exactly 4, identifying
only the middle selection. Native note, completion and action timelines match
exactly across source controls, with no timing tolerance or output adjustment.

The isolated transient has 104..107 nonzero frames, peaks of 126..612 units,
first audible output 12..15 frames after KON and last output 118 frames after
KON. ENDX and natural envelope zero both occur 161 frames after KON, before
gate release at 2123..2124 frames. All four natural-completion isolation
windows contain 3932 frames of exact transient silence and equality of the full
right mix to the continuing loop control. The two release gaps span
1354..1359 and 1997 frames, with audible looping output and no stale transient.
Final stereo tails are exactly zero.

Whole left GB streams match byte-for-byte. Maximum source residual is zero
units left and one right, below the unchanged four-unit budget. All scalar
controls match complete PCM and observations. Twenty-four shared-loop
fundamental measurements have maximum absolute pitch error 3.939101 cents
and minimum fundamental amplitude 361.008117 units, above the 100-unit minimum.
Both components persist in early and late returned-loop windows. Eight
single-tone held/GB measurements have maximum error 2.158957 cents, at least
sixteen periods and maximum period deviation 0.252794 frames. GB pulse error
alone is at most 0.064305 cents. Every pitch result is below the 10-cent limit.

Six public guard methods pass, covering owned data-only controls, export
refusal, both physical assignments, malformed source/completion/replay data,
silent or stale transients, missing/brief/wrong-pitched returns, peer dropout,
GB disturbance, timeline and source-sum errors. Detector guards also exercise
sine, square and triangle pairs at several phases, absent/weak/wrong-frequency
components and invalid window lengths. The final registered contract passes
in 16.90 seconds. Ten preceding focused CTests pass, covering one-shot,
interruption, polyphony, transition, combined/host/isolated pitch, shard runner
and firmware build/reproducibility contracts.

The matrix ran through the registered checker with native probe results from
this session. Twelve unchanged `both`, `gb` and scalar fixture executions were
reused after verifying exact fixture and PCM hashes and rerunning the final
metadata/PCM guards; eight newly isolated loop/transient controls were executed.
The full checker then required all four source comparisons and scalar parity.
It was not redundantly rerun through CTest. Probe build, Python compilation,
CTest matrix/contract label registration, shard planning, new-file whitespace
and `git diff --check` pass.

No new private-original, physical-hardware or independent-DSP comparison was
performed. Earlier complete acoustic, mapping and broad title matrices were
not rerun. Evidence covers this fixed owned bank, normal mapping/tuning,
admitted gates/pan/volumes, default modeled clocks and 48-kHz output. Other
banks, command phases, clock settings and presentation rates remain separate.

## Next step

Qualify audible replacement of an owned uploaded sound bank: stop or interrupt
the old bank, upload changed sample data and instrument mappings, then restart
and verify fresh PCM without stale tails while GB audio continues. Native
transaction coverage already exists; combined acoustic, queued-output replay
and scalar evidence should accompany it. Broader banks, independent hardware/
DSP comparisons and title compatibility remain separate qualification work.
