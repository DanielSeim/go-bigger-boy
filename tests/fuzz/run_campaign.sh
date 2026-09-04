#!/usr/bin/env bash
set -euo pipefail

# Run a reproducible parser campaign without writing generated inputs into the
# reviewed seed corpus. The CI workflow and local developers use this same
# entry point; environment variables make longer or shorter runs explicit.
FUZZER="${FUZZER:-./build-fuzz/gameboy_parser_fuzzers}"
SEED_CORPUS="${1:-tests/fuzz/corpus}"
RUN_CORPUS="${2:-fuzz-corpus-run}"
ARTIFACT_DIR="${3:-fuzz-artifacts}"
DURATION_SECONDS="${FUZZ_CAMPAIGN_SECONDS:-600}"
MAX_LENGTH="${FUZZ_MAX_LEN:-2097152}"
RSS_LIMIT_MB="${FUZZ_RSS_LIMIT_MB:-2048}"

if [[ ! -x "$FUZZER" ]]; then
    echo "fuzzer executable not found or not executable: $FUZZER" >&2
    exit 2
fi
if [[ ! -d "$SEED_CORPUS" ]]; then
    echo "seed corpus directory not found: $SEED_CORPUS" >&2
    exit 2
fi
if ! [[ "$DURATION_SECONDS" =~ ^[0-9]+$ ]] || ((DURATION_SECONDS < 1)); then
    echo "FUZZ_CAMPAIGN_SECONDS must be a positive integer" >&2
    exit 2
fi

mkdir -p "$RUN_CORPUS" "$ARTIFACT_DIR"
find "$SEED_CORPUS" -maxdepth 1 -type f -name '*.seed' \
    -exec cp -n {} "$RUN_CORPUS"/ \;

# A small margin prevents the outer timeout from cutting off libFuzzer while
# it is writing its final corpus statistics. Exit 124 is the expected timeout
# status; any other non-zero status indicates a finding or infrastructure
# failure and must remain visible to CI.
timeout_seconds=$((DURATION_SECONDS + 60))
set +e
timeout --preserve-status "${timeout_seconds}s" "$FUZZER" "$RUN_CORPUS" \
    -max_total_time="$DURATION_SECONDS" \
    -max_len="$MAX_LENGTH" \
    -rss_limit_mb="$RSS_LIMIT_MB" \
    -artifact_prefix="$ARTIFACT_DIR"/ \
    -print_final_stats=1
status=$?
set -e
if ((status != 0 && status != 124)); then
    exit "$status"
fi

echo "fuzz campaign completed: ${DURATION_SECONDS}s"
echo "review generated corpus in: $RUN_CORPUS"
echo "review crash artifacts in: $ARTIFACT_DIR"
