# Experimental native phrase-bank parsing

The isolated `score_phrase.asm` program follows real N-SPC bank pointers on the
native SPC CPU and feeds the muted [two-track scheduler](sgb-native-score-pair.md).
It is not bundled. SGB1/SGB2 program ROMs remain required for production playback.

The bank starts at `$2B00`, with its byte length supplied at `$20` (1..128).
Its first little-endian word points to a phrase list containing exactly two
pattern-table pointers followed by a zero word. Each pattern table contains
eight channel pointers: channels 2 and 3 must be present, and the other six
must be zero. Every referenced byte, including both bytes of each pointer,
must lie inside the supplied bank. Nonzero pointers must have high byte `$2B`.
Pointers can be relocated, shared or visited again; the bounded parser never
recursively follows a table or track. Unreferenced bank bytes are permitted.

Each of the four track streams has this supported grammar:

```text
[up to five setup commands] duration, 127, note-or-rest, 0
```

Duration is 1..127; note is `$80..C7`, and rest is `$C9`. The optional commands
are E0 instrument 2, E1 pan 10, ED track volume 127, E5 song volume 160, and
E7 tempo equal to the externally supplied tempo at `$12` (96 or 192). Commands
can appear in any order or repeat within the five-command bound. These setup
values are validated while muted; this experiment does not apply them to DSP
voices or qualify their execution timing. Inherited duration/articulation,
multiple timed events in a stream, additional patterns, calls, ties and broader
controls are rejected.

The SPC validates the entire phrase and all four streams before it starts the
timer or emits events. It writes the validated duration/opcode pairs to an
eight-byte native RAM cache at `$3100`, then uses independent track countdowns
to advance each pattern at the first track end. The host installs the raw bank;
it does not traverse pointers or prepare a timeline. The cache and parser state
are part of the complete APU save state. A malformed second pattern must reject
with no events, zero score ticks and silent PCM, just like a malformed first
pattern. The existing clock and flat-pair artifacts remain byte-identical.

Build and ROM-free checks:

```sh
python3 scripts/build_sgb_score_phrase.py --output /tmp/score-phrase.bin
cmake --build build-dmg-firmware --target gameboy_sgb_score_phrase_probe
ctest --test-dir build-dmg-firmware -R 'native_score_phrase$' --output-on-failure
```

The reproducible artifact is 550 bytes with SHA-256
`1c2572175ae8c458d3c551c0fe44220c3d102087e3a0634b9f2371617c055178`.
The production APU CPU, bus and timers execute owned source through an owned
IPL trampoline and direct RAM installation. The probe checks reset and
cross-instance save/load during parsing, after each pattern's event records,
and during countdowns. It checks silent PCM, unchanged DSP mute/reset, no
mailbox readiness and a stable terminal state. Reports use
`gbb-spc-score-phrase-v1`, with tick/channel/opcode/duration and actual event-log
half-cycle timestamps; they retain `qualification: false` and `playback: false`.
Sequential log writes do not claim simultaneous DSP key-on.

The ROM-free suite compares native records against the independent symbolic
N-SPC scheduler for all three established owned phrase banks, relocated tables
and reversed stream placement, shared pointers, rests, both tempos and duration
limits. It exercises every truncated prefix of the original fixture, null and
out-of-bank active pointers, nonzero inactive channels, bad phrase terminators,
invalid later streams, command-count/value limits and a track whose terminator
occupies the last permitted bank byte.

Private black-box validation:

```sh
python3 scripts/check_sgb_score_phrase_reference.py \
  --probe build-dmg-firmware/gameboy_sgb_score_phrase_probe \
  --trace build-dmg-firmware/gameboy_snes_65c816_apu_trace \
  --firmware-dir roms
```

The comparator runs the three owned raw phrase fixtures on both original
SGB1/SGB2 programs, then compares native logical events and the first-to-second
pattern interval with observed original voice-2/3 combined key-ons. The allowed
interval difference is 4096 SPC cycles for timer quantization and bounded RAM
logging latency; the two reference models must also agree within 2048 cycles.
This qualifies the bounded scheduling rule, not native DSP audio. Only sanitized
observations and hashes are exported. Private checks require external images;
no original code, samples or ROM assets are committed.

Six fresh checked runs agreed within 880 SPC cycles. The unequal first patterns
advanced at tick 16, and the equal long pattern at tick 32, followed by the
final 16-tick pattern. All eight native score CTest suites passed, including the
seven-case phrase suite; the bundled prototype reproducibility check retained
SHA-256 `8222797ddeec5681af61fda28c6afc241898887cd0f254ce2f691f30d4b13482`.

The separate [owned two-voice renderer](sgb-native-score-duet.md) now checks
combined onsets and clipped-note release using diagnostic full-duration gates.
General phrase lists, richer per-track execution, upload/whole-system
boot and production playback remain outside this diagnostic milestone.
