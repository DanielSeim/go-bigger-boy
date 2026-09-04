#!/usr/bin/env bash
set -euo pipefail

# Minimize a campaign corpus without modifying the reviewed seed directory.
# Promotion is deliberately opt-in: review the campaign artifact first, then
# rerun this command with --approve.
FUZZER="${FUZZER:-./build-fuzz/gameboy_parser_fuzzers}"
MAX_LENGTH="${FUZZ_MAX_LEN:-2097152}"
APPROVE=false

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "usage: $0 CANDIDATE_CORPUS REVIEWED_CORPUS [--approve]" >&2
    exit 2
fi
CANDIDATE_CORPUS=$1
REVIEWED_CORPUS=$2
if [[ $# -eq 3 ]]; then
    [[ $3 == "--approve" ]] || {
        echo "the optional third argument must be --approve" >&2
        exit 2
    }
    APPROVE=true
fi

if [[ ! -d "$CANDIDATE_CORPUS" ]]; then
    echo "candidate corpus directory not found: $CANDIDATE_CORPUS" >&2
    exit 2
fi
if ! [[ "$MAX_LENGTH" =~ ^[0-9]+$ ]] || ((MAX_LENGTH < 1)); then
    echo "FUZZ_MAX_LEN must be a positive integer" >&2
    exit 2
fi

if [[ $APPROVE != true ]]; then
    echo "dry run: review $CANDIDATE_CORPUS, then rerun with --approve"
    exit 0
fi
if [[ ! -x "$FUZZER" ]]; then
    echo "fuzzer executable not found or not executable: $FUZZER" >&2
    exit 2
fi

mkdir -p "$REVIEWED_CORPUS"
merge_dir=$(mktemp -d "${TMPDIR:-/tmp}/gbb-fuzz-merge.XXXXXX")
cleanup() { rm -rf "$merge_dir"; }
trap cleanup EXIT

"$FUZZER" -merge=1 "$merge_dir" "$REVIEWED_CORPUS" \
    "$CANDIDATE_CORPUS" -max_len="$MAX_LENGTH" -print_final_stats=1

promoted=0
while IFS= read -r -d '' input; do
    cp "$input" "$REVIEWED_CORPUS/"
    promoted=$((promoted + 1))
done < <(find "$merge_dir" -maxdepth 1 -type f -print0)
(cd "$REVIEWED_CORPUS" && sha256sum -- *.seed > MANIFEST.sha256)
echo "promoted $promoted minimized corpus inputs into $REVIEWED_CORPUS"
echo "updated corpus manifest in $REVIEWED_CORPUS/MANIFEST.sha256"
