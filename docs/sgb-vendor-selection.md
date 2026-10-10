# Uploaded-score selection workload and port timing

The [phase experiments](sgb-vendor-phase.md) exposed a large delivery-to-key-on
gap without identifying which part belonged to the SNES host or SPC. This
milestone adds controlled workload and request-timing fixtures and read-only
APU-port observations. The DB firmware image and production selection are
unchanged. No fixed delay has been added to the SPC interpreter.

## Controlled experiments

All sixteen fixtures use the same uploaded BRR, instrument-2 descriptor, tuning
688, tempo 45, duration 96, articulation 125 and first note 36. Score uploads
remain 256 bytes. Five workload profiles use no extra controls, sixteen or
64 redundant ED/127 volume controls before the first note, or the same controls
after it. The tail controls let a whole-score validation pass be distinguished
from execution of the prefix needed to start the note. These are valid owned
scores, not padding bytes interpreted as instructions.

Three additional profiles retain the baseline score and add 625, 1250 or 1875
iterations of an independently authored Game Boy spin loop before the second
SOUND request. The loop costs `28*n+8` Game Boy clocks. The experiment changes
request timing while retaining its score and state; it does not read or assert
the original driver's private timer state. Reports use actual delivered-request
intervals because transport may quantize the requested subframe offsets.

Each profile runs in two states: selection during the preceding long note
(sixteen Game Boy frames after the first request), and selection after completion
(100 frames). Both first and second selections must key on exactly once with
pitch/source/envelope words 1437/2/255/224/184. The final selection must complete
with FLG/KOF and both echo returns zero.

```sh
python3 scripts/build_sgb_vendor_selection_fixture.py \
  --profile tail64 --state completed --output /tmp/vendor-selection.gb
cmake --build build-dmg-firmware --target \
  gameboy_sgb_vendor_music_probe gameboy_sgb_music_timeline_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_selection_contract|score_vendor_selection)$' --output-on-failure
python3 scripts/check_sgb_vendor_selection_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe --firmware-dir roms
```

The private checker accepts `--model`, `--profile` and `--state` restrictions and validates
all requested fixtures before execution. It runs the opaque originals and the
independently written image using the same native host and bundled Game Boy/IPL
bootstraps. A successful check establishes completed measurements, not matching
selection behavior; `qualification` remains false.

On 2026-10-10 the final sixteen-frame public matrix and short contract pass in
314.85 seconds: 32 register captures plus four native lifecycle scenarios,
each with uninterrupted, restored and reset execution. Host/observer contracts
pass, as do the firmware build, report and shard-runner contracts. Shard
discovery retains this matrix exactly once among 87 and finds both probe targets.
Python compilation and whitespace checks pass.

All sixteen isolation cases complete on both originals and the replacement,
covering 64 captures. Active cases were rerun after widening their spacing;
unchanged completed-state measurements were retained, with final fixture,
program and firmware identities checked. The firmware SHA-256 remains
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
These tests use one native host; independent emulator, hardware and acoustic
qualification remain open.

## Observation contract

`SgbHost::debug_set_apu_port_observer` exposes the existing host CPU observer
without changing synchronization or execution. Like the DSP observer, its
binding stays outside snapshots and is retained by reset and destination load.
The observer contract requires identical PCM and complete state with and
without both observers, including cold reset, same-instance load,
cross-instance load and detachment. The short host contract also passes.

The timeline probe records only the first/last host APU-port write times and
four per-port write counts between delivery and first key-on for each request.
It never exports port values, private program bytes, RAM, tables, samples,
snapshots or PCM. Windows are bounded to sixteen requests; DSP events retain
their 128-event bound. Port times use the APU clock sampled at the host write
callback. They are diagnostic scheduling observations, not exact fractional
bus timestamps or a measured hardware latency. This precision is adequate to
separate millisecond/frame-scale forwarding from note setup. A window must be
ordered between its delivered request and key-on; the report fails on missing,
negative, out-of-range or misattributed observations.

An initial eight-frame active gap exposed an attribution limit on original
SGB1: with 64 prefix controls, the first note starts after the second request
has arrived. The latest delivered request is then insufficient to assign that
note to its command. The report rejects this case instead of calling it a lost
note or reporting a misleading delay. All isolation cases therefore use a
sixteen-frame active gap, retaining the active long-note state while allowing
both first key-ons to precede the next delivery. This does not qualify command
attribution for arbitrary rapidly queued requests.

## Baseline observations

On first selection, original host writes start about 16.6 ms after delivery.
The sixteen-frame active case starts its second port window at 1.170 ms on
SGB1 and 31.851 ms on SGB2; both issue two writes per port in that window.
The replacement also issues two per port, finishing within about 0.8 ms.
Original first/completed selections can have additional transactions. Write
counts alone therefore do not establish identical dispatch behavior.

| Baseline second selection | Last port write after delivery | Key-on after last port write |
| --- | --- | --- |
| Original SGB1, active | 17.062 ms | 27.063 ms |
| Original SGB2, active | 65.699 ms | 14.409 ms |
| Original SGB1, completed | 49.754 ms | 16.039 ms |
| Original SGB2, completed | 66.303 ms | 2.922 ms |
| Replacement, active | 0.762 / 0.725 ms | 11.604 / 11.590 ms |
| Replacement, completed | 0.308 / 0.308 ms | 11.454 / 11.455 ms |

The first original SGB1 selection ends its seven writes per port at 116.482 ms;
SGB2 ends its three at 49.927 ms. This explains much of the earlier model-specific
first-selection difference. The post-port interval is still phase-dependent.
It does not establish that every last observed write is the musical handoff,
or that the original protocols are identical to the replacement mailbox.

## Workload and phase findings

The completed-state profiles isolate startup workload with nearly identical
delivered-request spacing. Relative to the baseline, sixteen/64 prefix controls
add about 2.0/8.1 ms to original key-on time. Tail controls change it by at most
about 0.03 ms. In the replacement, prefix controls add about 8.8/35.4 ms and
tail controls about 4.4/17.7 ms. This is consistent with the replacement's
whole-score validation pass followed by live prefix parsing. That validation
continues to reject malformed tails before any audible prefix; it is not removed
to conceal the startup difference. Performance optimization remains deferred.

On SGB2 after completion, the 625-spin profile has essentially baseline request
spacing (1666.162 ms), while the 1250/1875 profiles both deliver at 1682.749 ms.
Those two offsets land on the same delivery frame, and both produce a 64.668 ms
second selection delay instead of baseline 69.225 ms. Thus distinct Game Boy
offsets need not produce distinct driver phases. These experiments constrain
observable timing, not the original private timer implementation.

A constant added delay would help some baseline selections and harm other
workloads: original SGB1's prefix-64 active selection already differs from the
replacement by only about 4.7 ms, versus about 31.8 ms for its active baseline.
The evidence supports a host/state-dependent dispatch contract rather than one
universal SPC delay.

Next, establish host command/acknowledgment milestones using controlled owned
music-ID changes. Use those boundaries to implement bounded experimental SOUND
dispatch pacing, retaining immediate STOP/upload ownership and testing rapid
reselection and save/load. Compare held-out phases and the pinned title before
accepting it. Port activity alone is not sufficient to identify every musical
handoff. Continuous note/rest phase, the unmatched short gate,
multichannel scores and acoustic/hardware qualification remain separate work.
