# Parser fuzzer seed corpus

These small, reviewable inputs seed `gameboy_parser_fuzzers`. The fuzzer uses
one input across all parser boundaries, so the corpus intentionally mixes
settings lines, trace envelopes, malformed records, and byte-like payloads.
libFuzzer may add minimized discoveries to a local or CI corpus; generated
artifacts are not committed automatically.
