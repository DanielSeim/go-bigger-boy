# Parser fuzzer seed corpus

These small, reviewable inputs seed `gameboy_parser_fuzzers`. The fuzzer uses
one input across its five parser boundaries (settings, traces, link packets,
save-state containers, and SGB commands), so the corpus intentionally mixes
settings lines, trace envelopes, malformed records, and byte-like payloads.
libFuzzer may add minimized discoveries to a local or CI corpus; generated
artifacts are not committed automatically. Review campaign artifacts before
promotion. Build the native `gameboy_parser_review` tool (it does not require
Clang or libFuzzer), then use the repository-root commands below to produce a
deterministic report containing
each input's hash, size, and outcomes at every parser boundary. Inspect that
report before promotion; `tests/fuzz/promote_corpus.sh` provides a dry run and an
explicit `--approve` step that verifies the report, then minimizes against
these seeds without overwriting them.
`tests/fuzz/check_corpus.sh` enforces the checked-in corpus's size, non-empty, and
duplicate-content invariants in CI and before local campaigns. `MANIFEST.sha256`
also makes reviewed seed changes explicit and reproducible. The campaign runner
invokes that validation automatically and refuses to use the reviewed directory
as its generated-output corpus. Minimized artifacts are normalized to `.seed`
names before the manifest is refreshed.

```sh
REVIEWER=./build/gameboy_parser_review \
  bash tests/fuzz/review_corpus.sh campaign-dir
bash tests/fuzz/promote_corpus.sh campaign-dir tests/fuzz/corpus
# After inspecting campaign-dir.semantic-review.tsv:
bash tests/fuzz/promote_corpus.sh campaign-dir tests/fuzz/corpus --approve
bash tests/fuzz/check_corpus.sh
```

Promotion also requires a built libFuzzer executable (default
`./build-fuzz/gameboy_parser_fuzzers`, override with `FUZZER`). The reviewer is
always built for native non-Android, non-Emscripten configurations; the fuzzer
requires `GAMEBOY_BUILD_FUZZERS` and Clang. Reports must be outside the candidate
directory and are verified against its current contents before approval. Default seed size limit
is 2 MiB (`FUZZ_MAX_LEN`). Hygiene and reported parser outcomes do not establish
that every promoted input is semantically safe; human review remains required.
