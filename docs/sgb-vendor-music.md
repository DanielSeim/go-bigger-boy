# Experimental uploaded game music with owned instruments

The separate DB diagnostic now plays a bounded single-channel N-SPC score
uploaded by an unmodified caller-owned Donkey Kong game. The Game Boy supplies
its own score and data through real JOYP/SOU_TRN transactions, the independent
SNES host uploads them through the bundled SPC IPL, and an independently written
SPC interpreter renders music through the existing DSP. No host-side score
conversion, patched game, pre-extracted score, injected SPC state or generated
host-side PCM is used.

This is experimental playback of one game entry with replacement timbres. It
is not production qualification or a proprietary sound match. The original
bundled prototype, the DA diagnostics, production selection and external-image
overrides remain unchanged. SGB1/SGB2 program ROMs remain required in production.
Performance optimization remains deferred.

## Reproducible build and runtime ownership

```sh
python3 scripts/build_sgb_vendor_music.py --output /tmp/vendor-music.rom
python3 scripts/build_sgb_vendor_music_fixture.py --output /tmp/vendor-music.gb
cmake -S . -B build-dmg-firmware
cmake --build build-dmg-firmware --target gameboy_sgb_vendor_music_probe
ctest --test-dir build-dmg-firmware \
  -R '^gameboy_sgb_(vendor_music_contract|score_vendor_music)$' --output-on-failure
```

The deterministic 256 KiB LoROM SHA-256 is
`0267b319ad503ea9ec57bb2ce9cbbbfb8ee0dd16ea872e63ec63e63e7a19ee28`.
The host is 1422 bytes; its SPC payload is 2919 bytes starting at `$0200`.
The strict assembler rejects overlaps, out-of-range operands/branches and
changed source hooks. Exporters refuse existing outputs. Neither this image
nor private input data is checked in or automatically selected.

| SPC region | Use |
| --- | --- |
| `$0200..0D66` | Code/padding, with reserved metadata and assets below |
| `$0400` | Cooperative score restart |
| `$0500..0508` | Trusted exclusive score end and counters retained across IPL |
| `$0600/$0700` | Owned sample directory and two authored looping BRR blocks |
| `$2B00..3AFF` | Uploaded score pool, with explicit supplied end |
| `$3B00..4CFF` | Caller-uploaded data retained separately, unused by this instrument profile |
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

Every transfer is preflighted before releasing ownership. Destinations outside
the score/data pools, source/destination overflow, score gaps/overlaps and entries
other than `$0400` reject before upload. The host appends a two-byte trusted end
descriptor at `$0500` using IPL transport; games cannot write that region.
Read bounds use that extent, never the contents of unprovided RAM. The driver
archives counters/mute state before cooperative IPL return and republishes its
DB signature only after restart and a fresh token-0 adoption handshake.

The uploaded caller assets **are not interpreted or admitted as instruments**.
IDs 2 and 10 map to the authored square and triangle blocks, respectively. That
policy enables a distributable resident sound source without copying original
resident samples, instrument tables or firmware. Supporting the game's uploaded
sample/descriptor bindings is a separate compatibility task. No proprietary
input is required to build or run the public tests.

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
the next event. STOP, reselection and upload discard the prior playback stack.

The following are **authored replacement policies**, not inferred vendor curves:

- An equal-temperament pitch table with register anchor 1068 at base note 24,
  used with both owned 16-sample loops. No sample-frequency equivalence is claimed.
- Linear pan points 0..20, multiplicative song/track volume, linear velocity and
  eight explicitly authored articulation fractions.
- Echo send projects the supplied mask onto the sole active channel-2 bit.
  Delay 0..15 uses the fixed owned echo pool and signed DSP volume/feedback bytes.
  Filter IDs 0..3 select an identity and three authored low-pass FIR responses,
  each summing to 127; the proprietary resident filter bank is not reproduced.

Natural completion and STOP mute/reset DSP output immediately. Echo release
fidelity, buffer-change timing, original pan/volume/gate curves, accurate
instrument tuning and fades need separate qualification.

## Public and private evidence

The public native tests exercise both models, scalar/native parity and combined
output, separate asset transactions, active upload, song switching, STOP,
mute/unmute, echo on/off and all four owned FIR responses. Malformed late notes,
channels, controls, pointers, truncated operands, work limits, protected-region
writes, exact 16-bit destination wrap and data-pool overflow must remain silent.
Every physical run compares full cold reset and actual cross-instance restored
continuation, including snapshots with unread output. Reports contain aggregate
counts and hashes; no PCM or snapshots are exported.

Validation on 2026-10-10: all seven public test methods pass, covering 59
model/mode/scenario runs, each with uninterrupted, restored and reset execution.
The final full-matrix CTest passes in 130.10 seconds. Six additional selected
CTest checks pass, including the prior original-firmware/transfer contracts,
build/reproducibility, the new short exporter contract and shard-runner contract.
CTest discovery assigns the new complete matrix to shard 0 among 85 full public
matrices. The original bundled prototype and DA image retain their hashes.
Python compilation, probe build and whitespace checks also pass.

The full public matrix runs in dedicated Linux firmware shards; the short
build/export contract retains ordinary platform/sanitizer coverage. The optional
private title gate is labelled `local;private-reference` and excluded from those
shards:

```sh
python3 scripts/check_sgb_vendor_music_title.py \
  --probe build-dmg-firmware/gameboy_sgb_vendor_music_probe \
  --game 'roms/Donkey Kong (JU) (V1.1) [S][!].gb'
```

Fresh final-image native runs on both models use the pinned unmodified game,
bundled model-specific GB bootstrap and bundled original IPL, with the existing
gameplay input script. Each runs to one billion master clocks, with uninterrupted,
restored and reset executions. The checker validates the resulting aggregate
reports. Each records two transfers, three adoptions, three SOUND requests,
two playback starts, one natural completion and two rendered notes. The counts
qualify this bounded observation window; they do not assert that both starts
naturally complete or that later gameplay is supported.

| Model | GB frames | Nonzero native frames | Restores / with unread output | Final output |
| --- | --- | --- | --- | --- |
| SGB1 | 2735 | 60373 | 612 / 27 | Muted |
| SGB2 | 2668 | 61025 | 612 / 18 | Muted |

Both runs produce 1489947 native frames and exact PCM/final-state parity after
reset and cross-instance restoration. No transfer or parser errors occur. Final
nonzero output precedes the clock boundary by more than one million clocks.
These checks establish execution of game-provided music through the independent
firmware and DSP. They do not compare original-program note schedules, acoustics,
independent emulators or physical hardware. `qualification` remains false.
The game hash and input-script pin are checked before the optional gate runs;
child diagnostics are sanitized and only explicitly allowed aggregate fields
are forwarded. Private ROMs, uploaded banks/assets, traces, snapshots and PCM
are never committed.

## Next substantive step

Compare this entry's selection, note onsets, gates and completion/stop behavior
against the private original as a black-box reference. Use those differences to
replace the provisional timing/control policies with measured contracts. Then
implement the uploaded instrument-10 descriptor/sample binding with owned public
fixtures and title evidence. Broader multichannel scores, calls, ties, effects
and release qualification remain separate milestones.
