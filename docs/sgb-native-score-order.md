# Experimental same-tick track-end priority

The isolated renderer now advances the pattern when channel 2 ends at the same
score tick as channel 3 reaches another note or rest. Channel 3 does not execute
that event or its preceding controls. This extends the
[clipped-peer diagnostic](sgb-native-score-peer.md), which still builds separately
with its preceding hash.

## Measured ordering and native scope

Owned four-pattern cartridges execute two duration-16 notes on each channel.
At tick 32 one channel ends with trailing ED64; the other reaches ED64 followed
by duration 8, articulation 63 and a note A0 or rest C9. The next patterns use
solo channel 2, solo channel 3, then both channels. All onsets remain 16 ticks
apart. Startup selects owned instrument 2, center pan, track volume 127, song
volume 160 and tempo 96. Initial articulation is 63 or 127.

The original DSP observations distinguish two cases:

- When channel 2 ends first, channel 3's note/rest and ED64 are skipped. Its
  later notes retain volume 7/7. Only the next pattern's channel-2 pitch 1200
  is written between the second and third KON groups.
- When channel 3 ends first, channel 2 executes ED64 and its timed event before
  the pattern changes. For the note case, pitch 1700 is written, then overwritten
  by the next pattern's pitch 1200 before the single channel-2 KON. There is no
  separate KON for pitch 1700. Later notes inherit track volume 64, producing
  volume 1/1. The rest case has no extra pitch write, and retains the ED64.

Both models agree on this bounded ordering. The native layer implements the
channel-2-first cases. The channel-3-first cases remain observation-only and
reject in silent rehearsal before any live event, instrument setup or audio.
Handling an executed event that is overwritten before KON needs explicit
scheduler, gate and log semantics; treating it as an independently keyed note
would contradict the measured register sequence.

`build_sgb_score_order.py` applies one checked source hook: `multi_end2` jumps
straight to pattern advancement. The timing-inheritance walker already checks
channel 2 first, so skipped channel-3 timing never becomes carry state. The
trailing-control helper executes only channel 2's ending controls. All source
bytes remain structurally checked before playback, including skipped events.
The independent symbolic scheduler's existing `end_priority=True` mode agrees
with this qualified case; its default and reverse-case rejection are unchanged.

The reproducible program is 4016 bytes, ending at $17AF, SHA-256
`2efc1a89303d11f373991727492284819dd0d2fdff8ab750d8b770c12773a56a`.
It retains the 4096-byte diagnostic bound, owned instrument/sample source,
2048-byte score bound, four patterns, eight cached events per track, 64 total
events and 2032-tick limit. Earlier image hashes and the bundled prototype are
unchanged. No timer restarts, pulse discards, synthetic releases or inserted
score delays are used.

## Validation

The shared physical probe uses schema `gbb-spc-score-order-v1` and retains
reset, state restore, cross-engine restore, interrupted envelope, PCM, source
and cache guard checks. Eight new tests cover note/rest skipping, volume and
timing carry, implicit-duration return, preceding clipped-peer cases, silent
rejection by the old image and of the reverse cases, pitch ordering, bounded
malformed traces, complete reference identities and deliberately false gate,
pitch and type metadata. All eight new tests and 28 preceding score, fixture
and scheduler registrations passed (29 CTest registrations total), along with
the bundled-image reproducibility check and `git diff --check`.

```sh
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_score_order_probe
ctest --test-dir build-dmg-firmware -R 'native_score_order$' --output-on-failure
python3 scripts/check_sgb_score_order_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_order_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms > /tmp/score-order-reference.json
```

The local reference matrix contains four cases, two articulations and both
models: sixteen original executions, eight qualified native comparisons and
eight observation-only executions. Each original child retains the
8-million-instruction, 180-second, 16 MiB trace and fewer-than-32768-row limits.
Only sanitized owned-voice DSP metadata and input hashes are exported; original
instructions, scores, instrument tables, samples and PCM are never copied.
The report is `gbb-score-order-reference-v1`; original-only measurement omits
`--probe`. Timing allowances remain 4096 SPC cycles for native/original and
2048 for intermodel onset intervals. All 96 qualified note gates are directly
observed KOF releases; no missing release is inferred. Maximum native/original
raw gate difference is 3460 SPC cycles, and maximum onset interval difference
is 2987. Maximum intermodel onset interval difference is 55 cycles.

This remains an opt-in diagnostic with qualification and playback false. It is
outside bundled and production selection, and SGB1/SGB2 program ROMs remain
required. These checks do not establish original PCM equivalence.

Next, qualify channel-2 note/rest execution before a same-tick channel-3 end,
including the overwritten pitch, inherited timing/mix, and the next pattern's
single KON. Then cover a next pattern that leaves channel 2 inactive, where an
accumulated note might survive instead of being overwritten.
