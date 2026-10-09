# Bounded owned three-song selection

The opt-in [owned transport integration](sgb-native-score-transport.md) now has
a separate CC mailbox variant for a fixed three-entry song directory. It
uploads an independently authored 2048-byte bank through SOU_TRN, validates
all three roots silently before publishing fresh readiness, and selects songs
1, 2 or 3 through SOUND. Every selected song is revalidated before DSP playback.

Qualification and production playback remain false. SGB1/SGB2 program ROMs
remain required. The bundled prototype and production external-image selection
are unchanged, and performance optimization remains deferred. This adds owned
multi-song transport evidence, not proprietary-bank, instrument, acoustic,
hardware or real-title qualification.

## Sources, layout and admission

```sh
python3 scripts/build_sgb_score_transport.py --multisong --output /tmp/directory-host.rom
python3 scripts/build_sgb_score_directory_fixture.py \
  --order 1,2,3 --active --output /tmp/directory-game.gb
cmake --build build-dmg-firmware --target gameboy_sgb_score_transport_probe
python3 tests/sgb_score_directory_tests.py \
  --probe build-dmg-firmware/gameboy_sgb_score_transport_probe
```

Both exporters refuse to overwrite files. The 256 KiB multi-song image is
SHA-256 `72765eb405abe1b52eaa594b73d3e0403f072812e414828d07fffc8f924ffc3b`.
The default CB build remains exactly
`220d292b4a41d07af3feb8e9375b57a78506b7a52bc2512d87ebc6de18d2850c`.
The standalone native engine remains 4096 bytes at SHA-256
`b27faaa73cb996f6b6fcc0a3bf48160ead49c90ff268c78a14a9e5e615c78e45`.

`firmware/sgb/score_directory_bridge.asm` reads the three little-endian root
words at `$2B00..2B05`, using directory offsets 0, 2 and 4. Roots must be distinct,
start after the directory, and permit at least a two-byte read within
`$2B00..32FF`. They may be unaligned. Each root then passes the existing native
pointer, stream, pattern, event, control, gate and complete silent rehearsal
guards. A bad unselected root or stream prevents readiness for the entire bank.
No permissive fallback substitutes another song.

The diagnostic variant replaces the initial phrase read-cursor instruction
with a same-width call to the directory helper. The native engine retains its
fixed `$0800..17FF` allocation and admission guards. Command dispatch remains
below `$0400`, restart enters `$0400`, auxiliary handlers occupy `$0508..05FF`,
and the directory helper occupies `$0600..07FF`. The interruption archive at
`$0500..0503` remains separate. Assembly rejects overlapping sections; the
combined payload still fits `$0200..17FF`.

Cold readiness retains the cooperative loader contract and does not admit
SOUND before bank validation. Restart clears readiness and initializes the
directory cursor. The bridge
performs three bounded native parses/rehearsals in order, discarding call frames
between passes. Only after all three succeed does it advertise `5A/CC/A5` and
accept the fresh token-0 ownership handshake. Each parse retains the 2048-byte
source, four-pattern, eight-expanded-events-per-track and 8192-read bounds;
startup now performs at most three such passes. Runtime selection performs one
pass. Clock, snapshot, PCM and child execution caps remain unchanged.

## Owned songs and commands

The owned bank has distinct roots `$2B20`, `$2B30` and `$2B40`. The songs share
the already admitted first two patterns and use independently authored final
pattern tables/streams. Their independent scheduler results are:

| Song | Ending | Final score tick |
| --- | --- | ---: |
| 1 | Consecutive duration-4 notes, articulation 63 | 72 |
| 2 | Duration-4 note followed by duration-4 rest | 72 |
| 3 | Duration-4 note followed by duration-8 note, articulation 127 | 76 |

The host admits song IDs 1..3 and explicit music stop 80, with zero effects and
attributes. Other IDs use the bounded stop-and-halt path. Control 2 stages the
actual song ID; control 0 computes its directory offset, discards the preceding
playback stack, and re-enters admission before starting a fresh score timer.
Active stop, selection and cooperative IPL behavior retain the preceding
interruption contract. Uploaded banks never overwrite their directory to
simulate selection.

D8 is the directory offset, D9/DA the root scratch, DB the last rendered song,
DC the admitted-root count, and DD..DF comparison scratch. These are outside
native engine scratch. The whole-host probe reads physical RAM and DSP fields
without injecting state. Save/load triggers include each admitted-root count
and selected-song change, plus the preceding upload/render/interruption phases.
A successful bank must reach count 3 and restore checkpoints after all three
admissions; rejected third roots stop at count 2.

## Public evidence and limits

All six public directory tests pass across 52 model/mode/scenario runs,
each comparing uninterrupted, restored and cold-reset execution. It covers
silent readiness; all three songs on SGB1/SGB2 in native, scalar and combined
modes; forward/reverse active switching; stop/resume; active repeated upload;
invalid IDs 0/4/255; zero, out-of-bank, directory-overlapping, final-byte and
aliased third roots; and an unadmitted third-song stream. Forward switching
also checks scalar and combined modes.

Within each model, the three songs must produce distinct real DSP PCM digests.
Native and scalar PCM must match exactly; combined output retains its own exact
lifecycle comparison and bounded resampling counts. Active commands must see
positive voice-2 ENVX and the expected interruption count. Completed scores
must reach their independently scheduled final tick and leave at least one
million physical master clocks of silent tail. Malformed banks must remain externally owned after transfer
and produce no audio. Invalid IDs must halt without starting a song.

No proprietary firmware, score, sample, instrument table or PCM is a test input.
The fixture uses the owned three-byte GB boot jump; title bootstrap is outside
this evidence. The finite profile still covers channels 2/3 and the preceding
restricted note/control/gate cases. It does not admit arbitrary song counts,
vendor directories, nested calls, arbitrary instruments or eight-channel music.

The preceding 54-case single-song integration matrix also passes with the
updated probe. All nine selected directory, transport, host, transfer, firmware,
fixture and native-engine CTest suites pass. The reproducible CLI exports,
bundled prototype hash check and `git diff --check` pass.

## Next step

The [bounded uploaded instrument profile](sgb-native-score-instrument.md) now
maps instrument 2 to an owned uploaded descriptor and BRR sample. The separate
[two-instrument profile](sgb-native-score-dual-instrument.md) adds ordered E0
replay and voice-local inheritance. The [multi-block sample profile](sgb-native-score-brr-chain.md)
adds bounded chains and checked loop points. The [D0 profile](sgb-native-score-instrument-profiles.md)
adds bounded uploaded envelopes/direct GAIN and tuning. The D1 profile below adds non-looping BRR samples.
Echo, broader controls, eight-channel scheduling and real-title qualification
remain separate unfinished contracts.

The [D1 one-shot profile](sgb-native-score-one-shot.md) now covers bounded
non-looping samples and natural completion. The [D2 profile](sgb-native-score-brr-profiles.md) adds bounded BRR filters/ranges.
The [D3 relocation profile](sgb-native-score-relocated.md) now validates starts/loops
inside fixed sample windows. The [D4 atomic-upload profile](sgb-native-score-atomic-upload.md) now prevents
stale score/sample reuse. The [D5 recovery profile](sgb-native-score-upload-recovery.md) now permits a
fresh complete upload after rejection while keeping SOUND blocked until
admission. The [D6 mapping profile](sgb-native-score-instrument-mapping.md) now
separates score IDs from owned sample slots. Next qualify instrument-10
chromatic pitch before extending the tuning contract.
