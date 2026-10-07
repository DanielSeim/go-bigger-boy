# Experimental owned two-voice phrase rendering

The isolated `score_duet.asm` renderer adds owned audio to the
[bounded native phrase parser](sgb-native-score-phrase.md). It follows the raw
song/phrase/pattern/track pointers on the SPC CPU, validates the entire bank,
then schedules two tracks and renders physical voices 2 and 3. It is not bundled;
SGB1/SGB2 program ROMs remain required for production playback.

The parser's two-pattern, one-event-per-track grammar is retained, restricted
to notes `$98`, `$99`, `$A4` and rest `$C9`, with durations 2..127 and articulation
127. Tempo remains 96/192. Supported setup-command values are checked by the
parser, but this diagnostic renderer uses its own fixed DSP setup: source 0,
direct gain 127, master volume 127, voice 2 left-only at volume 80 and voice 3
right-only at volume 80. Echo, noise and pitch modulation are disabled. Both
voices use an independently authored, looping one-block square-wave BRR sample.
No original sample or code is included.

Both voices are configured before one combined KON write. Its mask is 12 for
two notes, 4 or 8 for one note plus a rest, and no KON for two rests. Pitches are
1068, 1132 and 2140 for the three supported notes, using sanitized register
values established by the owned pitch fixtures. Every pattern transition
clears KON and writes KOF mask 12, releasing both voices, including the clipped
longer track. KOF is held across more than two DSP frames before the next
combined key-on. Completion also keys both voices off and disables the timer.

These are full-duration diagnostic gates, distinct from the single-track
calibrated articulation profiles. The experiment does not qualify original gate
lengths, envelopes, volume/pan laws, live controls or original PCM. Separate
stereo routing makes release from each owned voice observable without mixing
its peer into the same output channel.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_duet.py --output /tmp/score-duet.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_duet_probe
ctest --test-dir build-dmg-firmware -R 'native_score_duet$' --output-on-failure
```

The reproducible 2073-byte artifact uses checked assembly hooks and has SHA-256
`a201bc486cd106d4db28c0bd151ac86535f63acb20d21f2ef29995c377c2dac8`.
The muted phrase parser stays byte-identical. As with the earlier diagnostics,
installation uses an owned IPL trampoline and direct RAM loading, not a whole
SGB boot or upload qualification.

The probe executes the production APU CPU, bus, timers and DSP. It captures
actual combined KON/KOF edges, score ticks, both voice pitches, event-log
half-cycle timestamps and owned PCM statistics/fingerprint. At each KON it
checks source, gain, stereo routing and disabled echo/noise/modulation. Reset
and cross-instance save/load must reproduce every event, edge, completion
half-cycle and PCM fingerprint. Checkpoints cover parsing, record emission,
active audio and each KOF edge, including continuation into release or re-key.
After completion, 40000 physical halves of release must settle to silence.
Malformed later streams must reject before events, key-ons or nonzero PCM.

Tests include the three established raw phrase banks, first-pattern track
lengths swapped, rests in either or both tracks/patterns, tempo doubling,
minimum supported duration, relocated pointers and unsupported inputs.
For a longer first-pattern track followed by a rest, the corresponding stereo
channel must become exactly silent while its peer's second-pattern note remains
audible. The probe measures the second pattern from 20000 physical halves
(10000 SPC cycles) after its first event record until completion. This exceeds
the fixed-gain DSP release tail; it does not assert instantaneous silence at KOF.
Short patterns can have no samples in this measurement window.

Optional private black-box comparison:

```sh
python3 scripts/check_sgb_score_duet_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_duet_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

Six original SGB1/SGB2 runs execute the owned raw phrase fixtures. The comparator
matches actual native combined key-on masks and both pitches, aligns logical
events with the independent symbolic scheduler, and bounds both native event-log
and DSP key-on pattern intervals within 4096 SPC cycles of the reference.
Reference models must agree within 2048 cycles. Original source 2 is part of the
reference fixture contract; native source 0 remains owned and is deliberately
not compared for timbre or PCM. This check does not qualify original key-off
behavior. Only sanitized observations, hashes and owned PCM statistics are
exported; proprietary assets stay external.

Six fresh checked SGB1/SGB2 runs matched both combined masks and pitches, with
native DSP pattern-onset intervals within 1417 SPC cycles of the reference.
All nine native score CTest suites passed, including six renderer cases with
both clipped-side checks at tempos 96/192. The bundled prototype reproducibility
check retained SHA-256
`8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.

Native and comparison schemas are `gbb-spc-score-duet-v1` and
`gbb-score-duet-reference-v1`. All reports retain `qualification: false` and
`playback: false`. Next is bounded multi-event track execution, followed by
separate calibrated polyphonic gate/release validation. General phrase lists,
controls, upload/boot and production integration still need their own evidence.
