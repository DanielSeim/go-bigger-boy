# Experimental independently rendered native tracks

The isolated `score_poly.asm` integration adds owned two-voice audio to the
[bounded multi-event scheduler](sgb-native-score-multi.md). Each track can change
notes or enter a rest independently. It is not bundled; SGB1/SGB2 program ROMs
remain required for production playback.

The raw-bank grammar, pointer bounds, four-event track limit and whole-timeline
rehearsal remain in place. Rendering restricts notes to `$98`, `$99`, `$A4`,
rests to `$C9`, durations to 2..127 and articulation to 127. Tempo is 96/192.
The earlier setup-command values are validated, but the fixed diagnostic DSP
setup remains source 0 with owned square-wave BRR, direct gain 127, master 127,
voice 2 left-only at volume 80 and voice 3 right-only at volume 80. Echo, noise
and pitch modulation are disabled. Gates remain full-duration diagnostics;
original envelopes, gate lengths, volume/pan laws and timbre are unqualified.

Before updating voices at a score tick, the native scheduler computes a changed
mask from the independent countdowns: 4 for voice 2, 8 for voice 3, or 12 for
both. It clears prior KON data and asserts KOF only for changed voices, retaining
any already held rest bits. After a release wait exceeding two DSP frames, it
configures pitches for the due notes, clears their KOF bits and issues one KON
containing only those new notes. A due rest stays in release. An unaffected
sounding peer is excluded from both release and re-key. Simultaneous notes use
one combined key-on. Pattern transitions and completion release both voices,
including the longer clipped track. Ticks with no due events make no key writes.

Parsing and the ambiguity rehearsal finish before DSP setup or event emission.
A malformed later track, unsupported note/duration or same-tick end/new-event
boundary must reject with mute/reset still set, no live events and silent PCM.
No host callback parses or schedules the score. The SPC CPU performs all work,
using the native caches and timer clock; the muted multi-event artifact remains
byte-identical.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_poly.py --output /tmp/score-poly.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_poly_probe
ctest --test-dir build-dmg-firmware -R 'native_score_poly$' --output-on-failure
```

The reproducible 2073-byte artifact has SHA-256
`6202e7b38f6cee051d9cc0fa2eab64642ab99d8a19470ae7afb9e76a2e88b97c`.
Installation remains an owned IPL trampoline/direct RAM diagnostic, outside
whole-system boot and upload qualification. The production APU CPU, bus, timers
and DSP execute owned source.

The probe captures actual KON edges and newly asserted KOF bits, with changed
mask, complete held KOF mask, both pitches, logical tick and physical half-cycle.
The validator independently folds emitted note/rest events into expected mask,
pitch and held-release states. Each edge must occur at its event or completion
boundary within bounded instruction latency. At KON, the probe checks source,
gain, stereo routing and disabled echo/noise/modulation.

During single-voice updates with an already sounding peer, the probe checks that
peer's KOF bit stays clear and its actual ENVX readback stays at 127. This detects
an unintended peer release or restart, beyond checking written masks. Diagnostic
`peer_checks` count physical halves checked for each channel. The asynchronous
fixtures require substantial checks on both peers at tempos 96/192 and verify
single-voice KON masks explicitly. Tests also check clipped voices followed by
rests: after 10000 SPC cycles of release, that side must be silent while its peer
remains audible. Short second patterns can have an empty measurement window.

The seven-case ROM-free suite covers simultaneous and asynchronous changes,
rest/note transitions, inherited timing, both peer directions, relocated banks,
clipping, all-rest patterns, maximum 16 events/1016 ticks, malformed later data
and ambiguous boundaries. Full reset and cross-instance save/load reproduce all
records, key edges, peer-check counts, completion timing and owned PCM fingerprints.
Checkpoints include parsing, rehearsal, active audio, changed-voice release and
re-key. Completion drains 40000 physical halves and must settle to silence.

Optional private black-box comparison:

```sh
python3 scripts/check_sgb_score_poly_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_poly_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

Six fresh original SGB1/SGB2 runs execute the owned three-onset multi-event banks.
The comparator aligns logical records with the independent symbolic scheduler,
matches actual native combined KON masks and both pitches, and checks adjacent
native DSP-onset and event-log intervals against the reference within 4096 SPC
cycles. Reference models must agree within 2048 cycles. These controlled private
fixtures exercise simultaneous updates; asynchronous peer preservation is
checked with owned component execution and PCM, not qualified as original
firmware behavior. Native source 0 is not compared for original timbre/PCM.
Only sanitized register/timing observations, hashes and owned PCM statistics
are exported. Proprietary code, samples and ROM assets remain external.

The six checked private runs matched all three combined onset masks and both
pitches, with maximum native/reference DSP adjacent-interval difference 3802
SPC cycles. The muted multi-event artifact and bundled prototype remain
byte-identical. All eleven native score CTest suites and the existing
phrase-fixture suite passed. The bundled SHA-256 is
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.

Schemas are `gbb-spc-score-poly-v1` and `gbb-score-poly-reference-v1`; reports
retain `qualification: false` and `playback: false`. The separate
[measured independent gates](sgb-native-score-polygate.md) now cover ten
articulation-63/127 profiles with inheritance, peer-preservation and clipping checks.
Broader controls, general phrases, boot/upload and production
integration remain outside this milestone.
