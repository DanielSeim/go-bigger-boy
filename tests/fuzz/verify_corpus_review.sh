#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 CANDIDATE_CORPUS REPORT" >&2
    exit 2
fi
CANDIDATE_CORPUS=$1
REPORT=$2
if [[ ! -f "$REPORT" ]]; then
    echo "semantic review report not found: $REPORT" >&2
    exit 1
fi

temporary=$(mktemp "${TMPDIR:-/tmp}/gbb-semantic-review.XXXXXX.tsv")
cleanup() { rm -f "$temporary"; }
trap cleanup EXIT
"$(dirname "$0")/review_corpus.sh" "$CANDIDATE_CORPUS" "$temporary" >/dev/null
if ! cmp -s "$REPORT" "$temporary"; then
    echo "semantic review is stale; regenerate it and review the changed inputs" >&2
    diff -u "$REPORT" "$temporary" || true
    exit 1
fi
echo "semantic review verified: $REPORT"
