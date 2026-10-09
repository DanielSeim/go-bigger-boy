# Register-only asynchronous instrument envelope reference

Newly authored two-voice fixtures measure a held instrument-10 envelope while
its peer retriggers, rests or alternates instruments 10 and 2. Both voice
orientations execute on both private originals. A fourth case ends the peer
early and measures the held note's clipped release. This extends the
[paired-note reference](sgb-instrument-envelope-reference.md) for the next
[D8 qualification step](sgb-native-score-instrument-adsr.md).
Qualification and production playback remain false; SGB program ROMs remain
required. Performance optimization remains deferred.

## Owned fixtures and observations

`build_sgb_async_envelope_fixture.py` sends one newly authored pattern through
JOYP/SOU_TRN. The held voice uses E0 instrument 10, note 24, duration 64 and
articulation 127. Both voices use pan 10 and track volume 127; voice 2 sets song
volume 160 and tempo 96 exactly once. The peer uses duration 16 and articulation
127. Both start together; subsequent onsets affect only the peer.

| Case | Peer stream | Ending |
| --- | --- | --- |
| retrigger | Notes 24, 25, 24, 25 on instrument 10 | Duration-1 rest, terminator |
| rests | Note 24, rest, note 25, rest on instrument 10 | Duration-1 rest, terminator |
| switch | Note 24/instrument 10, 25/2, 24/10, 25/2 | Duration-1 rest, terminator |
| clipped | Note 24/instrument 10, duration-16 rest | Terminates at tick 32 and clips the held duration-64 note |

The held stream ends with a duration-1 rest and terminator. These are original
firmware fixtures; duration-1 rests are outside the native D8 bounded grammar.
Future native counterparts must preserve the compared notes and explicitly
account for a supported silent ending. All eight owned cartridge hashes are
pinned by the public fixture test, including voice reversal. Exporters refuse
overwrite.

The unchanged trace tool's `--voice-envelope-trace` mode records published ENVX
once per 32-kHz output sample, exactly 12000 samples per voice after the first
combined KON. Only DSP writes and published-register observations are parsed.
Private RAM and internal renderer-state rows are not retained. Subsequent KONs
may select a single voice; each voice's next onset independently bounds its
preceding envelope. Every note requires its own explicit KOF and release to
zero before its next onset or the observation-window end.

Source, pitch and ADSR/GAIN setup must match exactly at each onset. Instruments
10 and 2 use the previously measured descriptors and pitch words. Writes to the
held voice's pitch, source or envelope registers while it is active are counted
and must remain zero, even if they rewrite the same value. Noise on either
observed voice, extra/missing onsets, missing/duplicate samples, unequal peer
sample clocks and incomplete windows fail closed.

## Measured behavior and pinned bounds

The held voice's full ENVX anchors remain exactly
`0,0,2,6,18,38,82,125,113,99,79` at output-sample offsets
`1,4,8,16,32,64,128,512,2048,4096,8192`. It first reaches 127 at offset 197,
releases from 70 and reaches zero. Retriggers, rests and instrument changes on
the peer do not alter these observations. Both originals and both orientations
agree exactly; held setup-write counts remain zero.

| Observation | Measured SPC cycles across both models/orientations | Contract bounds |
| --- | --- | --- |
| Full held gate | 334491..335207 | 334000..336000 |
| Clipped held gate | 170746..170907 | 170000..172000 |
| Peer first-note gate | 72334..72998 | 72000..74000 |
| Peer middle-note gates | 73238..74147 | 73000..75000 |
| Peer fourth-note gate | 75718..76766 | 75000..78000 |
| Peer onset at tick 16 | 84365..85165 | 84000..86000 |
| Peer onset at tick 32 | 172424..172762 | 171000..174000 |
| Peer onset at tick 48 | 258441..258777 | 257000..260000 |

The later peer gates differ from the earlier paired two-note fixture. These
finite bounds apply to these exact streams; they do not define a general
timing law or justify broadening the native gate table.

Clipping preserves the held decay anchors through sample 4096, then releases
from ENVX 92. Sample 8192 belongs to its settled tail, not its active anchors.
The clipping case therefore checks the same held prefix and an earlier KOF,
with exact release metadata rather than a full-duration held comparison.

Instrument-10 peer notes release from 111. Their first and later unshifted
attacks peak at offset 197. The second retrigger and third switch-case note
peak at offset 198, with early anchors `8=0,32=16,128=80`; other instrument-10
peer attacks retain `8=2,32=18,128=82`. Instrument-2 peer attacks peak at offset
10, have `8=0`, and retain the preceding fast-envelope anchors. Instrument 2
releases from 110 on its first selection and 109 on its fourth-pattern-note
selection. Exact per-case anchors and phase choices are pinned, not inferred
solely from the note number.

Every observed attack reaches 127; decay and release remain monotonic.
Published release drops are exactly one unit, exactly two output samples apart.
All settle and remain zero until another KON. For release-start ENVX `n`,
observed zero falls within `64*n-2 .. 64*n+128` SPC cycles after the observed
KOF. The fresh measurements include two lower-edge observations: 5886 cycles
for `n=92` and 7039 for `n=110`. The lower allowance is explicitly two cycles;
the prior paired-reference checker retains its preceding bounds. No reported
timestamp is adjusted. Cross-model gate/onset differences are bounded by 1024
cycles, zero-time differences by 128; all other trajectory fields agree exactly.

## Reproduce and validation limits

```sh
python3 scripts/build_sgb_async_envelope_fixture.py --case switch \
  --held-voice 3 --output /tmp/asynchronous-envelope.gb
python3 scripts/check_sgb_async_envelope_reference.py \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace --firmware-dir roms
ctest --test-dir build-dmg-firmware --output-on-failure \
  -R 'sgb_async_envelope_(fixture|local_reference)$'
```

The checker covers sixteen primary runs, 60 per-note/voice trajectories and
384000 published ENVX observations. Four additional switch-case runs compare
sampler-off/on DSP write fingerprints through the same bounded window on both
models. Only bounded setup/trajectory summaries, witness digests and input
hashes leave temporary storage. The output schema is
`gbb-sgb-async-envelope-reference-v1`, qualification/playback false. No partial
report is emitted on failure.

The final private CTest passed all twenty fresh child runs in 103.31 seconds.
Sampler-off/on fingerprints agree exactly: 2530 DSP writes on SGB1 and 2239 on
SGB2. The seven public fixture/parser/contract/exporter tests passed in 2.488
seconds before the final CTest regression run. D7/D8 frozen-image and exporter
checks passed; the bundled prototype reproducibility check retains SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.
Eleven focused public/contract CTests passed in 71.42 seconds, including the
new seven-method public suite, preceding envelope/chromatic/pitch fixtures,
native envelope/chromatic playback, original build/reproducibility, host and
transfer contracts, and the DSP envelope contract. `git diff --check` passed.
Earlier paired private-reference and full D8 physical matrices were not rerun;
their fixtures, observer and firmware images are unchanged.

The unchanged bounds are 8000000 instructions, 180 seconds per child, a 16-MiB
trace file and fewer than 32768 rows. Private CTest registration requires all
caller-owned firmware inputs. Public tests use owned scores and synthetic
register streams, exercise malformed traces and summary mutations, and require
no private images.

The originals execute with this repository's DSP. This independently observes
original firmware control and setup, but does not independently validate DSP
arithmetic against hardware or another emulator. No original instructions,
instrument tables, samples, directory bytes, private scores or PCM are used as
implementation inputs or committed artifacts. Raw private traces and child
stdout/stderr remain temporary. No firmware, emulator core, scheduler, DSP
arithmetic or production path changes in this milestone. Native asynchronous
reset/save-load and owned-PCM replay have not yet been qualified by these checks.

## Next step

The [D8 owned-sample whole-host checks](sgb-native-asynchronous-adsr.md) now add
these streams, reversed slots, setup stability, bounded ENVX and exact
reset/save-load/owned-PCM replay. They retain a clipped song-end scheduling
mismatch against the original allowance and explicitly preserve native
latch-transition observations. Next, correct the bounded scheduling offset
before broader instrument/sample or production integration work.
