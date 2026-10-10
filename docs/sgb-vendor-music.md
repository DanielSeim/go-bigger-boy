# Experimental uploaded game music and instrument binding

The separate DB diagnostic now plays a bounded single-channel N-SPC score
uploaded by an unmodified caller-owned Donkey Kong game. The Game Boy supplies
its own score and data through real JOYP/SOU_TRN transactions, the independent
SNES host uploads them through the bundled SPC IPL, and an independently written
SPC interpreter renders music through the existing DSP. Instrument 2 can now
bind the game's uploaded BRR, directory, envelope and tuning; instrument 10
retains an independently authored resident source. No host-side score
conversion, patched game, pre-extracted score, injected SPC state or generated
host-side PCM is used.

This is experimental playback of one game entry with uploaded and replacement timbres. It
is not production qualification or a proprietary sound match. The original
bundled prototype, the DA diagnostics, production selection and external-image
overrides remain unchanged. SGB1/SGB2 program ROMs remain required in production.
Performance optimization remains deferred.

## Reproducible build and runtime ownership

```sh
python3 scripts/build_sgb_vendor_music.py --output /tmp/vendor-music.rom
python3 scripts/build_sgb_vendor_music_fixture.py --output /tmp/vendor-music.gb
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target \
  gameboy_sgb_vendor_music_probe gameboy_sgb_music_timeline_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_music_contract|score_vendor_music)$' --output-on-failure
```

The deterministic 256 KiB LoROM SHA-256 is
`5b64893a6cc81fb231e2e36fd36102770761ddfc21a738c2244577c87efaa47a`.
The host is 1622 bytes; its SPC payload is 4512 bytes starting at `$0200`.
The strict assembler rejects overlaps, out-of-range operands/branches and
changed source hooks. Exporters refuse existing outputs. Neither this image
nor private input data is checked in or automatically selected.

| SPC region | Use |
| --- | --- |
| `$0200..139F` | Code/padding, with reserved metadata and assets below |
| `$0400` | Cooperative score restart |
| `$0502..0508` | Counters retained across IPL |
| `$0510..0514` | Trusted score end, asset-present flag and sample end |
| `$0600/$0700` | Owned directory/samples; uploaded source-2 directory at `$0608` |
| `$2B00..3AFF` | Uploaded score pool, with explicit supplied end |
| `$3B00..4AFF` | Caller-uploaded BRR pool |
| `$4B08..4B0B/$4C3C..4C41` | Uploaded source-2 directory and instrument-2 descriptor |
| `$8000..F7FF` | Echo pool, fixed ESA `$80`, EDL 0..15 |
| `$FFC0..FFFF` | Bundled IPL overlay |

The title first transfers its 1619-byte score at `$2B00`, then separately sends
sample/directory/descriptor data at `$3B00`, `$4B08` and `$4C3C`. The new host
accepts that data-only sequence. Score chunks within each transaction must be
contiguous starting at `$2B00`; an asset-only update requires an earlier score.
A score replacement supplies a new exclusive end, so a shorter bank cannot read
old trailing data. Asset updates preserve the admitted transport extent of the
score. This is an explicit incremental data transport contract, separate from
DA's complete owned score/sample transaction contract.

Each asset transaction must provide a four-byte directory at `$4B08`, a six-byte
instrument descriptor at `$4C3C`, and contiguous sample chunks beginning at
`$3B00` and ending no later than `$4B00`. Duplicated metadata, gaps, missing fields
and other destinations reject. Score replacement invalidates prior asset binding;
a complete same-transaction binding may replace it. A data-only update preserves
the score extent. No absent asset bytes are read as a new instrument.

Every transfer is preflighted before releasing ownership. Destinations outside
the score/data pools, source/destination overflow, score gaps/overlaps and entries
other than `$0400` reject before upload. The host appends a five-byte trusted extent/asset
descriptor at `$0510` using IPL transport; games cannot write that region.
Read bounds use that extent, never the contents of unprovided RAM. The driver
archives counters/mute state before cooperative IPL return and republishes its
DB signature only after restart and a fresh token-0 adoption handshake.

The independently authored binding maps `$4C3C` to instrument **2**, correcting
the earlier assumption that its address represented instrument 10. Owned uploads
executed on the opaque original establish the six-byte layout: source ID,
ADSR1, ADSR2, GAIN, tuning high byte, tuning low byte. Source ID 2 is supported;
zero tuning rejects. The driver applies the uploaded envelope and scales its
pitch scale by the unsigned tuning word / `$0400`, flooring and saturating to
`$3FFF`. Uploaded instrument 2 uses explicit measured note words for notes
24..37; other notes and both owned resident sources keep the authored scale.
This bounds the correction to observed notes rather than inferring a universal
vendor interpolation algorithm.

Before publishing readiness, the SPC walks the complete BRR chain inside the
trusted sample extent. Every nine-byte block must fit. A looping END must point
to a visited header; a one-shot END ignores its unused loop word, including zero.
Nonterminal loop flags reject in this restricted profile. Prefix/trailing bytes
may exist but are never decoded. The validated four-byte directory is copied to
owned `$0608`; DSP DIR remains `$06` and SRCN becomes 2. No sample bytes are copied
from the original firmware or embedded in this repository. Without an admitted
upload, instruments 2/10 retain the authored square/triangle resident sources.

## Supported rendering policy

SOUND accepts music IDs 1..3, 0 to retain music, and 128 to stop it. Effect fields
accept only 0/128, since this profile has no effect voices. Attributes retain
the existing reserved-bit rejection and use bits 2..3 = 3 for immediate global
mute; other accepted values unmute. Effect pitch/volume fields have no target
in this profile. Fade timing remains unimplemented. Both dry and echo volumes
are muted, and mute survives data-only transfers.

The selected directory word supplies a phrase root. Up to eight finite patterns
may populate logical channel 2 only. Each pattern requires an explicit duration
before a timed event. Notes `$80..C7`, rest `$C9`, optional/inherited articulation,
E0 instrument, E1 pan, E5 song volume, E7 tempo, ED track volume, F5 echo send,
F6 send disable and F7 echo setup execute on the SPC. Unsupported channels,
ties, calls, phrase repeats and other opcodes reject. Validation traverses the
complete selected score before live DSP setup or any note; readiness alone
advertises the restricted driver, not validation of every song root.

Each pass has a 2048-byte-read budget, 128 timed-event limit and eight-pattern
limit. Every pointer/operand read checks the supplied exclusive end. A malformed
late event therefore cannot play a valid prefix. Rejection mutes output and
reports E2; this DB profile halts and requires reset. DA's recovery behavior is
unchanged and is not claimed for this separate interpreter.

The timer uses target 16 and a tempo/256 fractional phase, servicing mailbox
commands while waiting and reading scores. Tempo bytes 1..255 are experimental;
only the earlier measured subsets have independent timing evidence. Timing
starts at the first event after validation, and pending timer pulses carry into
the next event. STOP, reselection and upload discard the prior playback stack. Calibrated
intervals below have a separate bounded pulse-count policy.

The following are **authored replacement policies**, not inferred vendor curves:

- An equal-temperament pitch table with register anchor 1068 at base note 24,
  used with both owned 16-sample loops; uploaded instrument 2 additionally scales
  it by its supplied tuning word. Notes 24..37 substitute the measured uploaded
  scale below; other notes remain provisional.
- Linear pan points 0..20, multiplicative song/track volume, linear velocity and
  eight explicitly authored articulation fractions.
- Echo send projects the supplied mask onto the sole active channel-2 bit.
  Delay 0..15 uses the fixed owned echo pool and signed DSP volume/feedback bytes.
  Filter IDs 0..3 select an identity and three authored low-pass FIR responses,
  each summing to 127; the proprietary resident filter bank is not reproduced.

Natural completion now keys off all voices, holds KOF across a DSP polling
phase, clears KOF, silences both echo returns, and leaves FLG and the DSP running.
This preserves decoder/envelope release instead of resetting it. Explicit STOP,
upload ownership and invalid-input rejection retain immediate mute/reset.
Selection latency, a continuous phase model, arbitrary gate/pan/volume curves,
other-note tuning, echo acoustics and fades remain unqualified.

## Bounded pitch and gate corrections

The uploaded note scale for 24..37 is 1068, 1132, 1200, 1272, 1348, 1428,
1512, 1604, 1700, 1800, 1908, 2020, 2140, 2268 at tuning `$0400`. The first
thirteen words come from the existing owned chromatic register observations;
the last is constrained by the owned uploaded-binding fixtures. Fresh uploaded
fixtures compare all fourteen notes at tuning 688 with the original, and retain
base/half tuning and envelope-only controls. These are DSP register contracts,
not copied resident tables, samples, or an acoustic-frequency guarantee.

Two explicit calibrated profiles supersede the provisional gate for matching
notes. Unmatched tuples retain the earlier experimental fractional policy.

| Tempo | Articulation | Duration | Gate pulses | Event pulses |
| --- | --- | --- | --- | --- |
| 45 | 125 | 96 | 534 | 546 |
| 96 | 127 | 16 | 37 | Existing fractional duration |

A pulse is 2048 SPC cycles. The long profile uses independent 16-bit gate/event
countdowns and restarts fractional phase at its calibrated event boundary.
Following rests and unmatched events therefore start from that explicit phase.
The short profile retains continuous fractional duration. Counts and phase
policy are independently authored calibration choices, not recovered original
implementation constants. Both gates clear KOF after about 0.611 ms; natural
completion clears it after about 0.111 ms. Busy holds are bounded and do not
reinitialize DSP state or discard pending timer pulses.

The new owned fixture builder covers a long note with/without a following rest,
a three-note chain, short-note control, notes 24..37, explicit STOP, reselection
and active asset upload. The native register gate compares pitches exactly and
gates, onsets and completion within four milliseconds (two timer pulses), with
KOF-clear holds within 0.05 ms. This deliberately leaves phase differences
visible instead of claiming exact timing.

```sh
python3 scripts/build_sgb_vendor_timing_fixture.py --case entry-chain \
  --output /tmp/vendor-timing.gb
```

## Public and private evidence

The public native tests exercise both models, scalar/native parity and combined
output, separate asset transactions, active upload, song switching, STOP,
mute/unmute, echo on/off and all four owned FIR responses. Malformed late notes,
channels, controls, pointers, truncated operands, work limits, protected-region
writes, exact 16-bit destination wrap and data-pool overflow must remain silent.
Every physical run compares full cold reset and actual cross-instance restored
continuation, including snapshots with unread output. Reports contain aggregate
counts and hashes; no PCM or snapshots are exported.

The preceding owned-instrument milestone on 2026-10-10 passed seven public
test methods covering 59
model/mode/scenario runs, each with uninterrupted, restored and reset execution.
That milestone's full-matrix CTest passed in 130.10 seconds. Six additional selected
CTest checks pass, including the prior original-firmware/transfer contracts,
build/reproducibility, the new short exporter contract and shard-runner contract.
CTest discovery assigns the new complete matrix to shard 0 among 85 full public
matrices. The original bundled prototype and DA image retain their hashes.
Python compilation, probe build and whitespace checks also pass.

The uploaded-binding extension passes eight matrix methods covering 97 physical
model/mode/scenario runs with normal, restored and reset execution. The complete
matrix and exporter CTests pass in 177.81 seconds. A ninth public method separately
checks the reference summary, including a reselection KOF immediately before the
second key-on. Four earlier firmware/transfer and APU contract/PCM CTests pass;
the new short host-observer lifecycle contract also passes. New cases cover
uploaded envelopes, half/saturated tuning, relocated looping chains, fragmented
sample transport, one-shot samples with unused zero loop words, stale-binding
invalidation and silent rejection of malformed or incomplete assets.

The full public matrix runs in dedicated Linux firmware shards; the short
build/export contract retains ordinary platform/sanitizer coverage. The optional
private title gate is labelled `local;private-reference` and excluded from those
shards:

```sh
python3 scripts/check_sgb_vendor_music_title.py \
  --probe build-dmg-firmware/gameboy_sgb_vendor_music_probe \
  --game 'roms/Donkey Kong (JU) (V1.1) [S][!].gb'
```

The final bounded-timing image passes all twelve public methods. Coverage now
includes 119 model/mode/scenario runs with uninterrupted, restored and reset
execution, plus twelve single register captures. The full matrix and exporter
CTest checks pass in 584.37 seconds; the short observer and shard-runner
contracts also pass. The new completion/retain case confirms that a later
music-0 request cannot restore stale echo volumes after natural completion.

The preceding binding milestone used native runs on both models with the pinned unmodified game,
bundled model-specific GB bootstrap and bundled original IPL, with the existing
gameplay input script. Each runs to one billion master clocks, with uninterrupted,
restored and reset executions. The checker validates the resulting aggregate
reports. Each records two transfers, three adoptions, three SOUND requests,
two playback starts, one natural completion and two rendered notes. The counts
qualify this bounded observation window; they do not assert that both starts
naturally complete or that later gameplay is supported.

| Model | GB frames | Nonzero native frames | Restores / with unread output | Final output |
| --- | --- | --- | --- | --- |
| SGB1 | 2733 | 59329 | 612 / 39 | Muted |
| SGB2 | 2666 | 59983 | 612 / 32 | Muted |

Both runs produce 1489947 native frames and exact PCM/final-state parity after
reset and cross-instance restoration. No transfer or parser errors occur. Final
nonzero output precedes the clock boundary by more than one million clocks.
That milestone selected SRCN 2 and ADSR1/ADSR2/GAIN 255/224/184 on both
models, matching the opaque original's observed envelope setup. Its replacement
pitch was 1435 versus the original's 1437, so exact tuning remains unqualified.
These checks establish execution of game-provided music through the independent
firmware and DSP. The later register-only comparison below adds note-schedule observations;
these lifecycle checks do not qualify acoustics, independent emulators or hardware. `qualification` remains false.
The game hash and input-script pin are checked before the optional gate runs;
child diagnostics are sanitized and only explicitly allowed aggregate fields
are forwarded. Private ROMs, uploaded banks/assets, traces, snapshots and PCM
are never committed.

## Register-only original comparison

The new read-only DSP observer is a diagnostic binding outside snapshots. Public
engine checks require identical PCM and complete machine state with and without
the observer and preserve destination bindings across load. The timeline probe
bounds requests to 16 and changed gate/reset/key-on events to 128. It exports
only timestamps, command IDs and note setup registers; no RAM, samples, programs,
snapshots or PCM. The optional checker sanitizes child failures and compares
owned binding controls on both programs/models, then the pinned title if supplied:

```sh
python3 scripts/check_sgb_vendor_music_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_music_timeline_probe \
  --firmware-dir roms --timing --game 'roms/Donkey Kong (JU) (V1.1) [S][!].gb'
```

The baseline measured title sends music 1 twice. The second request interrupts the first
before its note gate, explaining two starts but only one natural completion.
The original and replacement each key on twice. The original's final note gate
is about 1065.7 ms; the preceding replacement gate was about 1022.5 ms.
The original's stop sequence keys all voices off and clears KOF again, while the
preceding replacement immediately muted/reset DSP. Selection latency also differs and
varies with the original model/driver phase. These are compatibility gaps,
not reasons to change the separately measured DA diagnostic profiles.

The preceding optional comparison passed all 16 executions: three owned binding
controls on both programs and models, plus each program/model title window.
Both originals produce pitches 4528/4800/9072 for the authored base binding and
exactly half those words for half tuning. That replacement produced
4528/4796/9052 and halves them, preserving envelope-only pitch independence.
The source/envelope controls match while interpolation remains approximate.

| Baseline title interval | Original SGB1 | Original SGB2 | Replacement SGB1/SGB2 |
| --- | --- | --- | --- |
| Final note gate | 1065.694 ms | 1065.694 ms | 1022.528 ms |
| All-voice key-off after final key-on | 1087.810 ms | 1087.817 ms | 1092.944 ms |
| Second request to key-on | 58.981 ms | 64.435 ms | 14.980 / 15.054 ms |

The bounded timing correction on 2026-10-10 matches all fourteen uploaded
pitch words at tuning 688 and corrects the earlier base/half/envelope binding
pitch differences. The expanded private checker passed 36 original/replacement
executions across both models: binding controls, five owned timing fixtures and
the title window. After the completion-state guard was added, the final image
was rechecked against those original timing measurements, verifying unchanged
original program hashes first.

| Current title interval | Original SGB1 | Original SGB2 | Replacement SGB1/SGB2 |
| --- | --- | --- | --- |
| Final note gate | 1065.694 ms | 1065.694 ms | 1066.400 ms |
| All-voice key-off after final key-on | 1087.810 ms | 1087.817 ms | 1090.854 ms |
| Second request to key-on | 58.981 ms | 64.435 ms | 15.194 / 15.301 ms |
| Final note pitch word | 1437 | 1437 | 1437 |

The title gate difference falls from about 43.2 ms early to 0.7 ms late.
Natural completion leaves FLG and KOF zero and silences echo returns. It does
not reproduce the original selection delay or establish a general phase law.
The final image's one-billion-clock title lifecycle checks also pass on both
models, with exact PCM and final-state parity after reset and cross-instance
restoration. SGB1/SGB2 respectively produce 59292/59942 nonzero native frames,
with 637 restores each (34/20 containing unread output). Both retain the
previous transfer, adoption, request and playback counters.

These bounded windows use the same native `SgbHost`, bundled model bootstraps
and input events. Packet spacing itself differs with host execution, so the
report anchors key-ons to their own delivered requests and gates to their own
key-ons. It does not present absolute cold-boot PCM alignment as sound parity.

The subsequent [phase/selection measurements](sgb-vendor-phase.md) extend the
owned sequences and expose cumulative phase and unmatched gate differences.
The subsequent [selection isolation](sgb-vendor-selection.md) identifies host
port forwarding and validation workload as latency components. Next, establish
host command/acknowledgment milestones for bounded SOUND dispatch pacing.
Explicit STOP release,
fades, multichannel scores, calls, ties,
effects and production release qualification remain separate work. Uploaded
binding and measured register pitches do not qualify waveform matching.
