# Recovery after owned score/sample upload rejection

The opt-in D5 diagnostic extends [D4 atomic admission](sgb-native-score-atomic-upload.md)
with a fresh-upload path after preflight rejection or semantic admission failure.
A rejected generation loses playback readiness and stays muted. A later complete
SOU_TRN can replace both objects and restore readiness without resetting the host.

Qualification and production playback remain false. SGB1/SGB2 program ROMs
remain required. The format still contains three song roots, two instruments,
a 2048-byte score and a 192-byte sample/descriptor object. Vendor banks and
hardware behavior are not qualified. Performance optimization remains deferred.

## Build

```sh
python3 scripts/build_sgb_score_transport.py --multisong --uploaded-instrument \
  --two-instruments --multiblock --instrument-profiles --one-shot --brr-profiles \
  --relocatable --atomic-upload --upload-recovery --output /tmp/recovery-host.rom
python3 scripts/build_sgb_score_recovery_fixture.py --kind asset-gap --repeat \
  --stop --output /tmp/recovery-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_recovery_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

The image SHA-256 is
`bcbe4765596df4db0232bb23526c804bc9f7812a796cd16b2457cf697769a357`.
All preceding flags are required. Exporters refuse overwrite. Earlier diagnostic
images and the bundled prototype retain their hashes. The SPC payload is 6465
bytes, ending at `$1B40`, within `$0200..1BFF`; the host payload is 1528 bytes.
The code cap has only 191 bytes remaining, which constrains further extensions.

## Recovery contract

| Failure | SPC action | Host action | Next complete upload |
| --- | --- | --- | --- |
| Preflight rejection while still owned | Acknowledge cooperative command 4, mute, invalidate readiness and clear both objects | Discard nested preflight call frames, record rejection and return to polling | Allowed |
| Complete upload with invalid contents | Mute, clear roots/selection, reset SPC call stack and return to cooperative polling | Recognize D5/E2/A5 before adoption, record rejection and release transfer-pending state | Allowed |
| Timeout after ownership handoff, or failed recovery acknowledgment | Existing terminal error path | Halt without guessing the remote loader state | Requires reset |

D5/E2/A5 advertises cooperative rejection, not playback readiness or an external
driver. The host records status 9, sets a playback block, and continues reading
packets. It suppresses SOUND, including STOP, without touching SPC input ports.
The SPC also refuses staged SOUND while roots are unadmitted; its blocked STOP
handler cannot turn rejection into ready status. DSP output remains muted.
Before returning to polling, the SPC consumes the current input token so the
retained IPL restart address cannot become a spurious command 4. On semantic
rejection the host also synchronizes its command counter to the SPC's consumed
IPL jump echo. The next increment therefore differs even when the final chunk
is one byte and the retained jump token is 3.

The next SOU_TRN passes the same complete-object preflight as D4. Before IPL
loading, both objects clear again. Every song root and sample descriptor must
validate before the host adopts the generation and clears the playback block.
Repeated failures never reuse old data, restore old roots or fall back to old
samples. There is no partial-upload resume or rollback.

The host polling stack is `$01FF` in native mode, with 16-bit indexes and an
8-bit accumulator. Preflight rejection returns to that known boundary after
discarding any nested validation frames. Semantic rejection likewise resets the
SPC stack to its cooperative-loop boundary. No return address survives as a
future retry's caller. These paths are specific to this bounded diagnostic host.

WRAM `$56` is the playback block, `$57` counts rejections and `$59` counts
suppressed SOUND packets. These eight-bit counters are observations, not
admission authority. Archive `$0504` retains D4's clearing/unpublished/published
phases. A cold reset clears recovery state and replays startup; snapshots retain
exact host, SPC, DSP and recovery state.

## Evidence

The independently owned 32 KiB cartridge switches actual GB VRAM payloads with
LCD disabled in VBlank. It contains at most three frames and eight commands.
The changed final generation replaces score controls, sample bytes, descriptor
addresses, modes, filters and voice settings. Tests inspect both entire data
regions after every clear, then compare final hashes and descriptor words.

The new suite covers 112 physical model/mode/scenario runs:

- All eighteen prior rejection cases followed by a complete changed generation
  on both SGB models, including seventeen framing/coverage failures and invalid
  semantic contents.
- Preflight and semantic failure in native, scalar and combined rendering;
  repeated rejection with intervening STOP; cold rejection and recovery;
  and both mixed preflight/semantic failure orders.
- Continued blocked SOUND/STOP without a valid retry, with zero PCM during the
  blocked interval and no roots or playback selection.
- Rejection at each of the three directory roots, both with a valid retry and
  without one, proving partial directory admission is revoked on both models.
- Cold repeated semantic failures ending with a one-byte sample chunk, in all
  three render modes on both models. A locally built pre-synchronization D5
  witness timed out on the third loader request (token 3); the synchronized
  host admits the fresh generation. No witness image is committed.
- Frozen D4 witnesses that still halt before the later retry, plus complete
  changed transactions without rejection.

Each physical run compares uninterrupted execution, snapshot restoration and
cold-reset replay for exact state and PCM equality. Recovery transitions, clear
edges and suppressed packets select additional snapshots. Native and scalar PCM
must match; combined output has bounded frame counts. Execution retains caps of
140 million clocks, 4096 restorations, 250000 PCM frames and 4096 output bytes,
with each child bounded to 90 seconds. New scenarios use at most 100 million
clocks. Rejection and suppression counts, stack bounds, complete zeroing and
readiness invalidation are checked independently of final playback.

The preceding twenty-one selected atomic-upload, relocation, BRR-profile,
one-shot, instrument, directory, transport, host, transfer, firmware, fixture,
native-engine and BRR/DSP contract suites pass with the expanded probe. These
retain 980 prior physical scenarios plus 370 relocation and 60 BRR-profile
independent decoder objects. The initial complete 22-suite run passed in
2155.18 seconds. After the D5-only token synchronization fix, all eleven final
recovery tests are rerun in four independent groups, covering all 112 scenarios.
All four groups pass: 518.59, 461.58, 337.62 and 209.57 seconds respectively.
They cover every final recovery test, including the token-alias regression,
without rerunning unchanged preceding images.
The CTest timeout is 2400 seconds to retain headroom for the additional cases;
per-child bounds are unchanged. CLI byte reproducibility, overwrite protection,
previous image hashes, the bundled prototype check and `git diff --check` pass.
No proprietary firmware, bank, sample, descriptor or reference PCM is used.
Original-firmware, original-decoder, hardware, acoustic and title qualification
remain outstanding. Production firmware selection and external overrides are
unchanged.

## Next step

Decouple score instrument IDs from DSP sample slots. Start with an explicit
owned mapping for the independently measured IDs 2 and 10, mapped to the two
existing sample slots, with unknown-ID rejection and per-voice inheritance.
Establish bounded storage and code space before implementing the mapping, then
validate upload/recovery and reordered selection on both models. More samples,
original instrument descriptors and acoustic/title qualification remain separate
gates.
