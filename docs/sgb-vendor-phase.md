# Experimental uploaded-score phase and selection measurements

The DB interpreter now has reproducible owned fixtures for longer note/rest
chains and repeated selections. This milestone measures compatibility gaps;
it does not change firmware timing, select a new production image, or qualify
sound parity. The firmware remains the bounded pitch/gate image documented in
[uploaded music](sgb-vendor-music.md).

## Physical experiments

`build_sgb_vendor_phase_fixture.py` writes eight independently authored Game Boy
cartridges. Each uploads its own single-channel score, BRR loop, directory and
instrument-2 descriptor through JOYP/SOU_TRN. The descriptor uses tuning 688.
The long chains contain eight duration-96/articulation-125 notes at tempo 45,
with or without duration-1 rests. Short chains contain 24 notes or twelve
note/rest pairs at tempo 96, duration 16, articulation 127. A mixed chain repeats
long note, short rest, duration-16/articulation-127 note, duration-3 rest four
times at tempo 45.

Three selection cases exercise active long-note reselection, short-note
selection, and selection after natural completion. Active cases deliver nine
music-1 requests, with gaps of 8, 9, 10, 11, 12, 16, 24 and 100 Game Boy frames.
The completion case delivers four requests with gaps of 100, 101 and 102 frames.
Reports record actual delivery intervals; requested frame gaps are not assumed
to be identical absolute timestamps across firmware programs.

```sh
python3 scripts/build_sgb_vendor_phase_fixture.py \
  --case long-rest-chain --output /tmp/vendor-phase.gb
cmake --build build-dmg-firmware --target \
  gameboy_sgb_vendor_music_probe gameboy_sgb_music_timeline_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_phase_contract|score_vendor_phase)$' --output-on-failure
```

The public matrix captures all eight cases on both models and runs the
long-note/rest chain through uninterrupted, cross-instance restored and cold
reset execution. The short contract checks deterministic exports, refusal to
overwrite, command/note ownership, cumulative phase extraction, and rejection
of malformed reports. No private input is needed.

On 2026-10-10 the full public matrix and short contract pass in 261.22 seconds.
The matrix contains sixteen register captures and two native lifecycle runs,
each lifecycle run including uninterrupted, restored and reset execution.
The final short report-contract checks also pass separately after adding strict
request counts and an independently constructed cumulative-phase example.
CTest shard discovery includes the new matrix exactly once among 86 public
matrices and discovers both probe build dependencies.

## Optional original measurements

```sh
python3 scripts/check_sgb_vendor_phase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe \
  --firmware-dir roms
```

This read-only check executes caller-owned `sgb1.program.rom` and
`sgb2.program.rom` opaquely, using the same native host, bundled model-specific
Game Boy bootstrap and independently written SPC IPL as the replacement.
`--model` and `--case` can restrict the experiment. Each capture is bounded to
500 million master clocks, 16 requests and 128 changed register events.
Only program/fixture hashes, delivered-request intervals, pitch words, gate and
completion intervals, final release registers and calculated timing differences
are exported. No program bytes, resident tables, RAM, uploaded assets, snapshots
or PCM are exported or committed. Child diagnostics are suppressed on failure.

The checker requires every owned request/note, matching pitch words, natural
completion with FLG/KOF zero and silent echo returns. A successful run means
that the bounded measurements completed. It deliberately reports differences
rather than asserting that their size qualifies compatibility; `qualification`
remains false. Selection delay is measured from each delivered request to its
first key-on. Cumulative phase is measured from the first key-on to every later
key-on, then compared between programs. Gate and completion differences remain
separate so that similar total durations cannot hide a different release schedule.

## Findings and next contract

Eight long notes with inserted rests accumulate about 4.2 ms of onset difference
on either model. Mixed chains expose shorter-note gate differences around
11–14 ms, despite small onset differences. These extend beyond the earlier
three-note calibration window. First-selection differences also vary with model
and sequence. A single fixed delay or a new continuous phase law is therefore
not supported by these observations.

All eight experiments complete on both models, covering 32 original/replacement
captures, with matching pitch words. The current image retains SHA-256
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
The original program hashes match the preceding timing milestone. These are
measurements using one native host, not independent emulator/hardware validation.

| Sequence | Maximum accumulated onset difference, SGB1 | SGB2 |
| --- | --- | --- |
| Eight long notes | 0.214 ms | 2.229 ms |
| Eight long notes with rests | 4.134 ms | 4.201 ms |
| 24 short notes | 2.257 ms | 2.172 ms |
| Twelve short note/rest pairs | 2.157 ms | 2.241 ms |
| Mixed long/short notes and rests | 2.144 ms | 2.248 ms |

| Selection after the first request | Original SGB1 delay | Original SGB2 delay |
| --- | --- | --- |
| Long-note case | 50.131–73.501 ms | 57.691–76.190 ms |
| Short-note case | 30.129–61.488 ms | 45.066–64.188 ms |
| After completion | 60.267–65.801 ms | 60.085–69.235 ms |

Replacement delays across these selection cases are 11.748–12.357 ms.
The first request is also distinct: originals take about 129.077 ms on SGB1
and 59.413 ms on SGB2 in the selection fixtures. The mixed sequence's maximum
shorter-note gate difference is 11.960/13.546 ms on SGB1/SGB2. Neither those
gates nor the longer rest-chain phase meet the earlier four-millisecond bound.

The subsequent [selection workload and port observations](sgb-vendor-selection.md)
hold the first note/state fixed and separate host writes from later key-on.
They expose host/state-dependent port activity and validation workload. Host
command/acknowledgment milestones are needed to implement dispatch pacing.
The unmatched
tempo-45/duration-16/articulation-127 gate
needs a separate measured contract. Longer note/rest evidence must remain part
of any subsequent phase-policy validation. Independent emulator/hardware and
acoustic qualification, multichannel scores, calls/ties, fades and effects
remain open.
