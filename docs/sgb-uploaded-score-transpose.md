# Original uploaded-score transpose

GBS6 extends [GBS5 bounded repeats](sgb-uploaded-score-repeats.md) with the `EA`
per-track transpose control. Its operand is a signed two-complement integer from
**-12 to +12 semitones**. The envelope uses ASCII `GBS6`, keeps the same canonical
phrase/track layout and mandatory 1..4 total plays, and advertises mailbox v10
(`5A/CA/A5`) only after validating the complete bank. Earlier envelopes reject EA.
This remains an original opt-in diagnostic format; vendor title data and vendor
instrument/song-table behavior remain unsupported.

Every base note must still be in 0..31. Validation adds the current transpose to
each new note and requires its effective index to remain in the same 32-entry
pitch table. Underflow and overflow reject with E2 before any adoption; notes
are neither wrapped nor clamped. Zero and both transpose endpoints are accepted.
Control operands outside -12..12, unavailable controls in older formats, and
truncated controls also reject with E2.

Transpose consumes no score time. It changes subsequent notes, not the currently
held DSP pitch. A tie retains that held pitch even if transpose changes during
the tie. Rest/held-note validation remains unchanged. Each track starts with
transpose zero, independently of the other track. Every phrase start, whole
sequence wrap and SOUND music restart restores zero before interpreting that
track's controls. A trailing transpose control does not leak into the next phrase
or pass. These are independently defined semantics, without vendor equivalence.

The current transpose occupies direct-page `$46`; contexts store it at `$59`
and `$69`. Validation uses `$3A`, resetting it for each track and phrase. The
apply helper is at `$2100`, signed operand validation at `$2120`, and effective
note validation at `$2160`. All three fit existing reserved code space, so the
contiguous reproducible startup payload remains 9525 bytes from `$0200`.
Whole-host save states preserve the transpose bytes alongside held DSP pitch,
cursors, timers, repeat counters, staged commands and uploaded data.

The strict packer adds single-field events such as `{"transpose": -12}` only
for `"format": "GBS6"`. It rejects booleans, fractions, strings and out-of-range
values, and independently checks every effective note while encoding each track.
The existing offline oracle checks fresh-track grammar and signed EA operands;
it reports raw controls and does not qualify playback. All previous schemas,
255-byte bank limits, 4096-byte SOU_TRN transport and no-overwrite rules remain.

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_transpose_example.json --output /tmp/resident-transpose.bin
ctest --test-dir build-dmg-firmware -R gameboy_sgb_resident_score_contract --output-on-failure
```

The original fixture changes the left track to +12 during a tie, then plays the
next note an octave higher and another at -12. A right track holds an independently
transposed note. The next phrase has no initial transpose, then leaves trailing
controls to exercise reset on the subsequent repeat. Actual SPC/DSP checks on
both models in native and combined audio measure the retained tie, octave ratios,
independent right pitch, phrase reset and repeat reset. They compare scalar
PCM/state, repeat cold reset and restore through pending controls and boundaries.
A separate fixture renders effective indices 0 and 31. Invalid signed operands,
effective pitches and older-envelope controls reject before readiness; stop,
restart, finite termination, repeated uploads and legacy restart remain covered.
No proprietary firmware, title-score or recorded-audio inputs are used.

The [packed-score inspector](sgb-resident-score-inspection.md) reports canonical
GBS1..GBS6 phrase barriers, repeats, control defaults and effective pitches as
deterministic JSON before an upload, including retained tie pitch and transpose
reset in this fixture.
