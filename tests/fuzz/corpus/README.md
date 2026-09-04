# Parser fuzzer seed corpus

These small, reviewable inputs seed `gameboy_parser_fuzzers`. The fuzzer uses
one input across all parser boundaries, so the corpus intentionally mixes
settings lines, trace envelopes, malformed records, and byte-like payloads.
libFuzzer may add minimized discoveries to a local or CI corpus; generated
artifacts are not committed automatically. Review campaign artifacts before
promotion. Build the native `gameboy_parser_review` tool (it does not require
Clang or libFuzzer), then run
`REVIEWER=./build/gameboy_parser_review ../review_corpus.sh campaign-dir` to produce a deterministic report containing
each input's hash, size, and outcomes at every parser boundary. Inspect that
report before promotion; `../promote_corpus.sh` provides a dry run and an
explicit `--approve` step that verifies the report, then minimizes against
these seeds without overwriting them.
`../check_corpus.sh` enforces the checked-in corpus's size, non-empty, and
duplicate-content invariants in CI and before local campaigns. `MANIFEST.sha256`
also makes reviewed seed changes explicit and reproducible. The campaign runner
invokes that validation automatically and refuses to use the reviewed directory
as its generated-output corpus. Minimized artifacts are normalized to `.seed`
names before the manifest is refreshed.
