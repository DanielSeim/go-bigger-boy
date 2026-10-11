# SPC reply publication and host readback

The [controlled dispatch experiment](sgb-vendor-dispatch.md) could observe
matching host reads but could not tell when the SPC made its reply available.
This extension records accepted SPC output-port writes through the real APU
bus, without changing firmware, command handling, timing or DSP arithmetic.
Performance optimization remains deferred.

## Observer and report contracts

`SgbHost::debug_set_spc_port_write_observer` reports an accepted write's port,
value and physical APU half-clock timestamp. The bus publishes its output and
physical RAM before notifying. Host writes, DSP echo writeback and other SPC
RAM/DSP accesses do not notify this observer. It has a separate binding from
the existing RAM, DSP and host-port observers.

The audio scheduler owns the physical callback and supplies the SPC clock.
Host bindings remain outside snapshots and survive cold reset, same-instance
load and destination-instance load. The observer must not mutate execution;
callers own the context lifetime. Bus reset clears the raw binding, like other
physical bus callbacks; scheduler reset reinstalls its wrapper.

The timeline probe's optional `--spc-replies` mode includes the preceding
host-write/readback observations plus a parallel `spc_reply_handshakes` array.
For each host write it records first/last SPC output-write times, first/last
times the published byte equals that host write, and total/equal output-write
counts. Every publication is assigned to the latest host write on the same
port and in the same delivered request, before its first key-on. No matching
write means absent publication evidence, even when host readback matches.

The probe retains sixteen requests, 128 host-write records, 128 changed DSP
events and at most a million reads or publications per record. Overflow fails
without a partial success report. Neither the written nor read byte is
exported. The private checker retains a 260-million-master-clock bound,
240-second child timeout and 32-KiB child stdout limit. Firmware instructions,
RAM, raw packets, tables, assets, snapshots and PCM remain private.

`gbb-sgb-vendor-reply-v1` retains all distinct-note selection, setup, preceding
state and final-release contracts. It additionally rejects mismatched record
identities, extra fields, malformed counts and publication times crossing a
later host write or the key-on boundary. First matching host-read time minus
first matching SPC-publication time measures the observed polling gap.
A negative result remains visible: an earlier matching read does not prove
acknowledgment of a later publication. Missing matching publications remain
null rather than being assigned a fitted timestamp. Equality alone still
does not establish the original protocol's musical handoff.

## Reproduce

```sh
cmake --build build-dmg-firmware --target \
  gameboy_sgb_vendor_music_probe gameboy_sgb_music_timeline_probe \
  gameboy_sgb_music_observer_tests gameboy_snes_audio_host_tests
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_reply_contract|score_vendor_reply|music_observer_contract)$' \
  --output-on-failure
python3 scripts/check_sgb_vendor_reply_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The checker supports `--model`, `--profile` and `--state` restrictions and
executes caller-owned originals opaquely. It uses the four preceding authored
fixtures, both models, the same native host and bundled GB/SPC bootstraps.
No private bytes are implementation inputs or committed artifacts.
`qualification` remains false: successful observation does not qualify timing
compatibility, acoustics, an independent emulator or physical hardware.

The public matrix checks all eight fixture/model pairs against ordinary
timeline output without reply collection and retains four exact reset/load
PCM/state replays. The observer contract separately checks state/PCM neutrality,
destination binding preservation, reset/load, detachment and scalar/batched
publication timestamp equality. The short report contract stays in platform
and sanitizer CI; the full matrix stays in dedicated firmware shards.

The DB image remains SHA-256
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
Production still requires SGB1/SGB2 program ROMs. Continuous score phase,
the unmatched short gate, multichannel scores, effects, fades and acoustic
qualification are separate work.

## Private observations, 2026-10-11

All sixteen fresh original/replacement captures complete across the four
fixtures and both models. Distinct-ID pitches and source/envelope setup,
preceding active/completed states and final release all pass. The original
program hashes match the preceding dispatch experiment, and the DB image
identity is unchanged.

Across port-0 records with a matching publication, the original's first
matching output write occurs 0.389..26.865 ms after the associated host write.
When the original subsequently reads a matching value before key-on, that
first read occurs another 1.925..14.645 ms after the first matching publication.
The replacement's matching publications take 0.071..0.608 ms, followed by
matching reads within 0..0.006 ms. These outward-rounded ranges cover both
models, repeated/distinct IDs and active/completed states.

Representative second selections, using the first port-0 record with both
a matching publication and a subsequent matching read:

| Original model/state | Host write to matching publication | Publication to matching host read |
| --- | --- | --- |
| SGB1 active | 13.948 ms | 1.927 ms |
| SGB1 completed | 3.163 ms | 13.402 ms |
| SGB2 active | 19.199 ms | 14.635 ms |
| SGB2 completed | 6.563 ms | 9.997 ms |

Original selections can also publish matching replies without a matching
host read before the first key-on. The output retains those publication
times and a null read-gap field. Constant-field replies on other ports do
not identify a musical handoff. Multiple publications within one host-write
interval are counted; the first matching publication is a marker, not proof
that the later read acknowledges that particular transition.

This separates a real polling contribution from reply-publication latency.
It does not justify assigning the whole selection gap to either one, copying
the original wire protocol, or imposing a universal added delay.

## Next step

The [controlled field/pitch-setup experiment](sgb-vendor-handoff.md) now
provides counterexamples to treating a matching port value as acceptance.
Identify the selection boundary using actual score-directory reads and
correlate it with reply publication and host readback before designing bounded
experimental dispatch pacing. Validate that policy against held-out phases,
rapid reselection, immediate STOP/upload ownership, exact reset/save-load and
the pinned title. No pacing law or firmware change is accepted here.

## Validation evidence

All four public test methods pass in 227.494 seconds, including eight reply
captures, eight ordinary-timeline controls and four exact state/PCM replay
scenarios. Five focused bus, engine, observer and dispatch/reply CTests pass
in 3.95 seconds. The strengthened observer test additionally restores an
in-progress upload into a separately bound destination and compares the
remaining reply timestamps/values, state and PCM exactly; its final rerun
passes in 2.63 seconds. Scalar/batched, cold-reset, same-instance load and
detachment checks also pass.

All sixteen fresh private captures pass. Four shard-runner tests pass and
discovery registers the reply matrix exactly once among 89 public matrices,
with both probe targets. Python compilation, native builds and whitespace
checks pass. Earlier full firmware matrices, pinned private title playback,
Windows execution, independent emulators and physical hardware were not
rerun. The firmware image and production selection remain unchanged.
