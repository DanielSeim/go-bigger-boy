# Experimental instrument-2 envelope integration

The isolated envelope renderer applies the measured instrument-2 DSP setup to
both voices of the [chromatic mix renderer](sgb-native-score-mix.md). It uses
an independently authored sample and native ADSR instead of diagnostic direct
gain. This remains outside bundling and production selection; SGB1/SGB2
program ROMs are still required.

## Native setup and provenance

| Register | Both voices 2/3 |
| --- | --- |
| SRCN | 2 |
| ADSR1 | `$8F` |
| ADSR2 | `$6F` |
| GAIN | `$B8` |

These exact register values were previously measured through owned fixtures
and are pinned again at every original and native onset. GAIN is written even
though ADSR selects the envelope mode. The owned directory starts at `$1000`;
source-2's entry at `$1008` points to the existing newly authored looping BRR
block at `$1010`. No original samples, directory entries, instrument tables,
private scores or firmware instructions are implementation inputs.

`build_sgb_score_envelope.py` composes the existing independently written
assembly with strict setup hooks and adds the source-2 directory slot. The
assembler still checks section overlap and instruction/branch bounds. The
2602-byte image at `$0800` has SHA-256
`4cf02fde17d9af55a3583fc5d643fe09cee04b0828694fbd1c630a092abb8e1c`.
The preceding mix image, authored waveform, measured pitch words, gate pulse
counts and clock setup retain their existing bytes or contracts.

All mix/parser limits remain: 1..128 raw bank bytes, two patterns, channels
2/3, one nonnested call per track with counts 1..3, 1..8 expanded timed events
per track, 32 events/2032 ticks, and at most 32 executed controls per expanded
track. Only instrument 2 is accepted. Pan, track volume, shared startup song
volume, articulations 63/127, all ten gate profiles, rests, first-end clipping,
prevalidation and silent rejection retain their prior behavior.

## Envelope and lifecycle checks

The full APU probe observes read-only ENVX every physical half-cycle. Each
note gets a bounded summary associated with its actual KON and KOF:

- Attack visits zero and reaches ENVX 127, followed by positive decay below
  127. After the startup window, the envelope cannot increase while the note
  remains active, including when its peer changes notes or controls.
- Released ENVX decreases monotonically. Observable release drops are one
  ENVX unit, separated by 128 physical half-cycles: the underlying release
  subtracts eight envelope units per DSP sample and ENVX hides four low bits.
- An uninterrupted tail must reach zero. Fast retriggers may interrupt a tail;
  the validator uses the observed next KON and a bounded release duration to
  distinguish that case. A new KON must again visit zero and reach the attack
  peak. The probe also observes the final 40000-half-cycle quiet tail.
- Settled expired voices stay at ENVX zero while their active peers retain a
  positive, nonincreasing envelope and unchanged volume/pitch/setup. Opposite
  exclusive pan endpoints additionally check physical PCM silence and audible
  peer output.

The inherited whole-engine/cross-engine checkpoints, reset replay and restored
continuations compare setup snapshots, per-note envelope summaries, actual
KON/KOF timestamps, event logs, completion and owned PCM fingerprints.
Malformed/truncated banks still reject before events or audio. Metadata guards
reject missing trajectories, incorrect register setup, false attack/decay
claims, missing release observations and invalid types/timestamps. Supplying
the preceding direct-gain image to this component probe also fails.

ADSR decay uses the global rate clock. Different command prefix lengths can
therefore change PCM peaks slightly even with identical reduced volume pairs.
Both measured reductions must lower owned PCM amplitude; constant centered
settings retain exact stereo equality and constant endpoints retain silent
inactive lanes. Dynamic left/right writes retain the mix renderer's outgoing
tail limitation. Gate and onset timing allowances are unchanged.

The twelve ROM-free tests include the complete mix regression contract, all
thirteen octave notes on both articulations, every measured gate profile,
repeats/returns, asynchronous peer preservation, clipped notes and rest-only
silence. These bounded note durations exercise attack, decay and release;
they do not qualify the eventual long-held sustain phase or dynamic ADSR
changes. Other instruments and proprietary timbre/PCM equivalence remain
outside this milestone.

```sh
python3 scripts/build_sgb_score_envelope.py --output /tmp/score-envelope.bin
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_envelope_probe
ctest --test-dir build-dmg-firmware -R 'native_score_envelope$' --output-on-failure
```

## Fresh private reference evidence

```sh
python3 scripts/check_sgb_score_envelope_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_envelope_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
```

The owned 125-byte `pan-calls` and `song-calls` fixtures are unchanged from the
mix milestone, including their documented cartridge hashes. Eight fresh runs
cover both original models and both articulations, with 128 directly observed
per-voice KOF releases. Source/ADSR/gain, chromatic pitch and volume pairs match
exactly at every compared onset. Native/original onset spacing differs by at
most 1974 SPC cycles, gate lengths by 2918, and original model onset intervals
by 464. Existing 4096-cycle native/original and 2048-cycle inter-model allowances
remain unchanged.

The private trace records firmware DSP writes, not the DSP's internal ENVX
updates. Thus original comparisons establish register setup and gate/onset
timing; the native envelope trajectories have separate component evidence.
No original envelope-curve or PCM equivalence, physical-device or independent
emulator comparison is claimed. Original files are executed opaquely, with the
existing 8000000-instruction/180-second child limits and bounded temporary
traces. Exported evidence contains only sanitized register/timing metadata and
hashes; raw traces and proprietary assets are not committed.

The envelope suite and eighteen preceding score/fixture regression suites
passed. The bundled prototype passes its reproducibility check and is unchanged.
Schemas are `gbb-spc-score-envelope-v1` and `gbb-score-envelope-reference-v1`,
with qualification and playback false. Installation remains an owned IPL
trampoline/direct RAM diagnostic; whole-system upload, boot, title playback
and production integration remain ahead.
