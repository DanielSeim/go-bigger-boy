# First audible bank after cold rejection

This gate extends [mixed-failure recovery](sgb-bank-mixed-audio.md) to startup
without a previously admitted bank or native note. The cartridge's initial
upload is defective. SOUND start/stop attempts remain blocked; a complete valid
retry admits the first bank and enables the first owned source-3 triangle note.
GB audio windows continue through the blocked commands, retry and playback.

Firmware, emulator core, DSP, mixer and state format are unchanged. This is
bounded owned diagnostic evidence; general replacement qualification and
playback remain false. Production still requires private SGB1/SGB2 program
ROMs. No proprietary firmware, instructions, scores, descriptors, samples or
PCM are implementation inputs or committed artifacts. Performance optimization
remains deferred.

## Owned cartridge and controls

`build_sgb_bank_cold_audio_fixture.py` exports a deterministic 32768-byte GB
cartridge with two physical 4096-byte payloads: the existing `asset-gap` or
`bad-root` defective bank at `$4000`, followed by the complete owned fresh bank
at `$5000`. The automatic initial SOU_TRN uploads the defective payload before
any marked command. There is no initial valid bank or old native loop. Fresh
publication uses the reversed IDs 10/2 and selected source-3 triangle, with
exact owned score/sample hashes. Header/global checksums and overwrite refusal
remain required.

Six actual GB stages have VBlank delays 64, 16, 4, 4, 64 and 12:

1. Start the left-routed GB pulse without sending SOUND.
2. Attempt blocked SOUND start.
3. Attempt blocked SOUND stop.
4. Upload the complete valid bank through GB VRAM/SOU_TRN.
5. Start the first admitted native note.
6. Stop it and mute GB routing.

The fixture sends four SOUND packets; the first two must be suppressed. No
probe injects commands, RAM writes or DSP state.

Three controls isolate the two sources. `both` keeps the GB pulse and fresh
native sample audible. `native` changes only the GB routing write from `$10`
to zero; its uploaded bytes are identical to `both`. `gb` keeps GB routing but
silences only fresh BRR data; code, score, descriptors, maps and BRR headers
remain identical to `both`. Every profile retains the identical defective
initial payload. The native-only routing change must not alter event timing.

| Observation | `asset-gap` | `bad-root` |
| --- | --- | --- |
| Initial rejection | Host preflight | Driver semantic admission |
| Initial error | 1 | 2 |
| Completed transfers before retry | 0 | 1 |
| RAM after rejection | Zero score/sample objects | Exact defective objects |
| Clear generations | 0, 0 | 0, 1 |
| First valid publication generation | 1 | 2 |
| Final rejections / suppressions | 1 / 2 | 1 / 2 |
| Final native note count | 1 | 1 |

The completed-transfer counter includes semantic-failure handoffs but not
preflight failures. Neither failure may create a valid publication or note.

## Native and acoustic guards

The shared native probe accepts the failure name followed by `cold`. Its
report retains the shared native schema and adds `cold_rejection: true`.
Cold reports omit the warm active-loop mute/gap fields. The aggregate checker
uses `gbb-sgb-bank-cold-audio-v1`, retaining `qualification=false` and
`playback=false`.

Native capture forbids any note before stage 5. There must be two complete
score/sample clears, one valid publication, one note onset/release/zero and
three rejection/suppression observations. Every clear zeros all 2048 score and
192 sample bytes and invalidates readiness. Failure observations require exact
error, completed transfers, blocked state, readiness/roots, mute/KOF, rejection/
suppression counts and known RAM hashes. The initial rejection must precede
stage 1; both suppressed commands must fall within their actual SOUND windows.

Valid retry must be published and admitted before the first note. Unblocking
before publication fails immediately; after admission, every host step must
preserve unblocked status, zero error and final rejection/suppression counts.
Final score/sample RAM must match fresh publication. The one note must use
source 3 and its changed pitch, then produce a real final KOF/zero with positive
ENVX and the selected voice's ENDX bit.

Each case compares complete host state, PCM and observations after cold reset
and in-place save/load. Event and unread-output replay masks cover all six
stages, both clears, the valid publication, all three failure events, admission
and note onset/release/zero. Scalar `both` must match complete batched PCM and
observations. The existing 100-million-clock, 1536-restore, one-million-PCM-byte,
16-KiB-report and 150-second-child bounds remain in force.

Every profile requires exact right-channel silence before the first native
note, including startup, rejection, blocked SOUND, retry and admission. The
GB-only control's right channel must stay zero throughout; the native-only
control's left channel must stay zero. Combined left/right streams must equal
the corresponding isolated sources exactly, and whole stereo residual
`both - native - gb` must stay within four units. No alignment, gain fitting or
rescaling is used. All native timelines must match across controls, allowing
only the intentional GB routing byte and fresh sample hashes to differ.

Early/late 2048-frame native windows retain the 10-cent pitch limit. Five
model-specific GB pulse windows cover first GB start, both blocked commands,
retry clearing and fresh playback. No GB output is admitted before its first
routing/trigger command, allowing four boundary frames for the IO/marker
sequence. Final stereo tails must be zero; no clipping is admitted.

## CI and reproduction

The complete matrix has 32 cases: two models, two physical voices, two failure
types, three source profiles and scalar `both`. Each case includes reset and
restored execution. CI partitions these into complete 16-case tests
`gameboy_sgb_bank_cold_audio_gap` and `gameboy_sgb_bank_cold_audio_root`, each
with a 3600-second timeout and `sgb-firmware-extended` label. They run on separate
dedicated Linux shards. The public four-method contract stays in platform/
sanitizer jobs. The extended set now has 76 tests across the existing eight
Linux shards.

```sh
cmake --build build-dmg-firmware --target gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_audio.py \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/check_sgb_bank_cold_audio.py --kind bad-root \
  --probe build-dmg-firmware/gameboy_sgb_bank_replace_audio_probe
python3 scripts/build_sgb_bank_cold_audio_fixture.py --kind bad-root \
  --profile native --voice 3 --output /tmp/cold-retry.gb
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R '^gameboy_sgb_bank_cold_audio_contract$'
```

## Validation evidence

All 32 native cases and eight source comparisons pass on SGB1/SGB2, voices 2/3,
both failure types and all three profiles, including exact scalar parity for
`both`. Every case captures 223492 frames with 754–786 restores, including
292–324 restores with unread output. Reset and save/load reproduce complete
state, observations and PCM. Both 16-case CI filters reproduce their exact
partitions of the full result, with fixture and PCM hashes rechecked.

All eight whole-stream source residuals are zero on both channels. The 16
native pitch windows have at least 20 periods and maximum absolute error
2.821 cents; the 40 GB pulse windows have at least 19 periods and maximum error
0.956 cents. Native output remains exactly silent for the first 136757–139787
frames, until its first admitted note. Fresh ENVX is 97 in every run; admission
follows publication by six frames. There is no clipping.

The four public guard methods pass. They check exact two-payload placement,
source-control byte differences, deterministic checksummed exports, overwrite
refusal, both models/voices/failures, premature notes or publications, stale
error/RAM/admission state, wrong handoff counts, incomplete clears or replay,
early native PCM, source leakage/mixing, silent/brief/wrong-pitched first notes
and common GB dropouts.

Nine focused CTests pass: cold, mixed, tail, recovery, rejection and
stopped/active replacement contracts, shard runner and firmware build/
reproducibility. The new cold contract passes in 14.75 seconds. The shared
native probe reproduces the preceding SGB1 voice-2 `root-gap` mixed recovery's
entire metadata and PCM exactly, including reset/restored executions.

CTest discovery assigns the two complete cold matrices to dedicated Linux
shards 1 and 2; the public contract stays unlabelled. Probe build, Python
compilation and whitespace checks pass. Earlier complete native matrices,
private references and broad title playback were not rerun.

## Evidence limits and next step

Owned same-renderer controls establish internal consistency. Independent DSP
arithmetic, hardware timing, private-original acoustics and broad title
compatibility remain unqualified. [Cold one-byte recovery](sgb-bank-cold-tail-audio.md) adds single/repeated
semantic rejection with token synchronization. Cold mixed failures, other
semantic defects, command phases, banks, clocks and presentation rates remain
separate work. The DA image remains SHA-256
`1cf5d56ed87e9407d7d3e3e8753157f23544a29f2876587c41771496ee0b5d82`.

Next, cover cold mixed preflight/semantic failure orders before the first valid
bank, retaining silence, fresh playback, GB continuity and queued-output replay.
