# Controlled music-ID dispatch and readback observations

The [selection workload experiment](sgb-vendor-selection.md) showed that host
forwarding contributes substantially to selection latency. This experiment
uses independently authored music IDs with distinct pitches and observes
host port reads as well as writes. It changes no firmware, dispatch policy,
emulator timing, DSP arithmetic or production selection. Performance
optimization remains deferred.

## Fixtures and observation contract

Four fixtures select IDs 1, 2, 3 or repeat ID 1 three times, either during an
active note or after completion. Each song has one channel-2 note, with the
same uploaded instrument-2 binding, tempo 45, duration 96 and articulation
125. IDs 1, 2, 3 use notes 36, 35, 34 and pitch words 1437, 1357, 1281 at
tuning 688. The score, BRR and descriptor are authored inputs; no private
resident data is used. Active requests are sixteen GB frames apart;
completed requests are 100 frames apart. Distinct notes check that each
delivered command selected its intended root rather than merely retriggering.

The timeline probe's optional `--port-handshakes` mode records at most 128
host writes before key-on, across at most sixteen delivered requests. For
each write it records the request, port, timestamp, subsequent equal/unequal
read counts, and first/last read and equal-read times. Reads belong to the
latest write on the same port within the same delivered request; observations
stop at that request's first key-on. Each record has a million-read bound.
Overflow rejects the report rather than truncating a successful result.

Equality compares host readback with the preceding host write internally.
Neither byte is exported. Port bytes, firmware instructions, RAM, tables,
samples, raw packets, snapshots and PCM are excluded. Equal readback is a
**candidate acknowledgment**, not proof of a musical handoff: reads can match
coincidentally, and these ports may carry separate command and status fields.
No inferred protocol token or private driver algorithm is implemented.

Timestamps use the APU clock at the host bus callback, retaining the preceding
instruction-boundary precision limitation. They are scheduling observations,
not exact fractional bus timing or hardware latency measurements.

The checker requires the intended three requests, three correctly owned
key-ons with matching pitches/source/envelope, the intended active/completed
state before both later requests, and natural final release with FLG/KOF and
echo returns zero. It rejects extra/private fields, malformed counts and
timestamps, read intervals crossing a subsequent write, and missing request
observations. No partial successful private report is emitted on failure.
Its schema is `gbb-sgb-vendor-dispatch-v1`; `qualification` remains false.

## Reproduce

```sh
cmake --build build-dmg-firmware --target \
  gameboy_sgb_vendor_music_probe gameboy_sgb_music_timeline_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_dispatch_contract|score_vendor_dispatch)$' \
  --output-on-failure
python3 scripts/check_sgb_vendor_dispatch_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The private checker supports `--model`, `--profile` and `--state` filters.
It executes caller-owned originals opaquely through the same native host and
bundled GB/SPC bootstraps as the replacement. Each capture has a
260-million-master-clock bound, 240-second timeout and 32-KiB stdout limit.
Only bounded derived register/timing metadata and image identities leave
the temporary execution directory. Successful observation does not qualify
timing compatibility, acoustics, another emulator or physical hardware.

The public matrix runs all four fixtures on both models, verifies identical
ordinary timeline output with and without readback collection, and checks
exact reset/save-load state and PCM replay for both distinct-ID states on
both models. Its short fixture/report contracts remain in platform and
sanitizer CI; the full matrix runs once in the dedicated firmware shards.

The DB image remains SHA-256
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
SGB1/SGB2 program ROMs remain required in production. Continuous note/rest
phase, the unmatched short gate, multichannel scores, fades, effects and
acoustic/hardware qualification remain separate work.

## Validation and findings, 2026-10-11

All sixteen fresh private captures complete: four fixtures, both models and
both original/replacement programs. Every distinct-ID selection has the
expected pitch, source and envelope, and both preceding-state checks and
final release pass. The original program identities match the preceding
[selection milestone](sgb-vendor-selection.md).

Distinct-ID original selection delays, measured from delivered SOUND to key-on:

| Model/state | First ID | Second ID | Third ID |
| --- | --- | --- | --- |
| SGB1 active | 129.076 ms | 44.139 ms | 60.967 ms |
| SGB1 completed | 129.077 ms | 65.813 ms | 66.981 ms |
| SGB2 active | 59.412 ms | 80.122 ms | 67.174 ms |
| SGB2 completed | 59.413 ms | 69.249 ms | 66.805 ms |

The maximum selection-delay change versus the repeated-ID control is
0.020 ms on SGB1 and 0.027 ms on SGB2. These fixtures therefore provide no
evidence that changing the music ID itself explains the much larger latency
gap. The replacement retains approximately 11.7..12.4 ms selection delays.

Each original selection has one equal-read observation on port 0. Its delay
from the associated write is 15.872..16.881 ms on SGB1 and
16.560..33.834 ms on SGB2. The original observes the ports together near a
later frame boundary; the replacement instead polls its token on port 0
repeatedly while waiting. The recorded equality time is the **host's read**,
not the instant the SPC made its reply available. A whole-frame observation
delay cannot be attributed to SPC processing from these measurements alone.

The complete public suite passes in 227.793 seconds: eight flagged captures,
eight ordinary-timeline controls and four exact reset/save-load replay
scenarios. All three test methods pass. Five focused host, observer,
selection, phase and dispatch CTests pass in 30.64 seconds; the four shard
runner tests pass and discovery registers this matrix exactly once among
88 public matrices, with both probe targets. The build and whitespace
checks pass. Earlier full firmware matrices and the pinned private title
were not rerun; no firmware image or production path changed.

## Next step

Observe the SPC's outbound port writes without exposing private values or
changing execution. Pair those reply-publication times with the host writes,
host readback and distinct-note key-ons. That can distinguish actual command
service from waiting for the host's next poll. Preserve the ownership and
snapshot/observer contracts, then use supported boundaries to design bounded
experimental dispatch pacing and validate held-out phases, rapid reselection,
STOP/upload ownership and the pinned title. No pacing law is accepted here.
