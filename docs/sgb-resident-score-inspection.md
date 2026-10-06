# Inspecting original resident scores

Pack an authored score, then inspect the exact upload before using it with the
opt-in original SGB program prototype:

```sh
python3 scripts/build_sgb_resident_score.py firmware/sgb/resident_transpose_example.json --output /tmp/resident-transpose.bin
python3 scripts/inspect_sgb_resident_score.py /tmp/resident-transpose.bin
```

The inspector emits deterministic JSON for canonical GBS1 through GBS6 uploads.
It requires exactly 4096 bytes, the bank at `$2B00`, and an exact round trip through
the strict authoring packer. Header fields, phrase and track pointers, supported
controls, fresh-track ties, effective pitch bounds, the `$0400` final jump and
zero padding must all match. Other valid firmware encodings, generic SOU_TRN
transfers and vendor banks are outside this tool's accepted input. Input reads
stop at 4097 bytes; errors exit with status 2 and no JSON output.

The report includes the upload SHA-256, bank size, advertised mailbox version,
total plays and an expanded finite timeline. Each phrase has a start/end tick;
each track has its own end tick and DSP voice (channel 0 uses voice 4, channel 1
uses voice 3). The next phrase starts at the later track ending, including rests.
Patterns containing only controls take zero score ticks. At most four phrases
and four plays produce at most sixteen timeline entries.

Every event includes its source SPC address and score-relative tick. Timed events
include duration, effective pitch index (null for rests), and current source,
pan, direct gain and transpose. A new note adds transpose to its base pitch; a
tie retains the preceding held pitch even after a transpose control. Controls
consume no score ticks. Track controls reset to source 1, pan 10, gain 80 and
transpose 0 at every phrase and repeat boundary. The report describes a fresh
music restart, without intervening SOUND stop, restart or effect commands.

For the supplied GBS6 fixture the phrases span ticks 0..16 and 16..20, then
20..36 and 36..40 on the second play. The first track's effective indices are
12, 12 (tie), 24 and 0 in each first phrase. Its subsequent phrase starts at 12.
The timeline totals 40 ticks, or a nominal 640000 microseconds.

One score tick corresponds to the prototype's 16 ms timer interval. Nominal
duration excludes upload/startup, command timing, SPC instruction latency and
DSP release tails. This is static authoring inspection: `qualified` and
`playback` remain false, and it does not establish PCM, cycle or vendor-title
equivalence. The existing real SPC/DSP integration suite separately checks
playback on SGB1 and SGB2. Production still requires external SGB program ROMs;
the independent SPC IPL is already bundled.

```sh
ctest --test-dir build-dmg-firmware -R 'gameboy_sgb_(resident_score_inspection|resident_score_contract|score_decoder_contract)' --output-on-failure
```
