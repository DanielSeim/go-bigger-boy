#!/usr/bin/env bash
set -euo pipefail

# Produce deterministic parser outcomes for human review of campaign inputs.
REVIEWER="${REVIEWER:-./build-fuzz/gameboy_parser_review}"
MAX_LENGTH="${FUZZ_MAX_LEN:-2097152}"

if [[ $# -lt 1 || $# -gt 2 ]]; then
    echo "usage: $0 CANDIDATE_CORPUS [REPORT]" >&2
    exit 2
fi
CANDIDATE_CORPUS=$1
REPORT=${2:-${CANDIDATE_CORPUS}.semantic-review.tsv}

if [[ ! -d "$CANDIDATE_CORPUS" ]]; then
    echo "candidate corpus directory not found: $CANDIDATE_CORPUS" >&2
    exit 2
fi
if ! [[ "$MAX_LENGTH" =~ ^[0-9]+$ ]] || ((MAX_LENGTH < 1)); then
    echo "FUZZ_MAX_LEN must be a positive integer" >&2
    exit 2
fi
if [[ "$(realpath -m "$REPORT")" == "$(realpath -m "$CANDIDATE_CORPUS")"/* ]]; then
    echo "review report must be outside the candidate corpus" >&2
    exit 2
fi
if [[ ! -x "$REVIEWER" ]]; then
    echo "semantic reviewer not found or not executable: $REVIEWER" >&2
    exit 2
fi

mapfile -d '' candidates < <(find "$CANDIDATE_CORPUS" -maxdepth 1 -type f -print0 | sort -z)
if (( ${#candidates[@]} == 0 )); then
    echo "candidate corpus contains no regular files: $CANDIDATE_CORPUS" >&2
    exit 1
fi
for path in "${candidates[@]}"; do
    size=$(stat -c '%s' -- "$path")
    if ((size == 0)); then
        echo "empty candidate input: $path" >&2
        exit 1
    fi
    if ((size > MAX_LENGTH)); then
        echo "candidate input exceeds FUZZ_MAX_LEN ($MAX_LENGTH): $path" >&2
        exit 1
    fi
done

temporary=$(mktemp)
cleanup() { rm -f "$temporary" "$REPORT.tmp"; }
trap cleanup EXIT
"$REVIEWER" "$CANDIDATE_CORPUS" > "$temporary"
mkdir -p "$(dirname "$REPORT")"
{
    echo "# gbb semantic corpus review v1"
    echo "# file<TAB>sha256<TAB>bytes<TAB>settings_entries<TAB>trace_valid<TAB>trace_records<TAB>trace_errors<TAB>link_packet<TAB>save_state<TAB>sgb_command"
    rows=0
    while IFS=$'\t' read -r file bytes settings_entries trace_valid trace_records trace_errors link_packet save_state sgb_command; do
        [[ -z "$file" || "$file" == \#* ]] && continue
        path="$CANDIDATE_CORPUS/$file"
        [[ -f "$path" ]] || { echo "reviewer reported missing file: $file" >&2; exit 1; }
        actual_size=$(stat -c '%s' -- "$path")
        [[ "$actual_size" == "$bytes" ]] || { echo "candidate changed during review: $file" >&2; exit 1; }
        hash=$(sha256sum -- "$path" | cut -d' ' -f1)
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$file" "$hash" "$bytes" "$settings_entries" "$trace_valid" \
            "$trace_records" "$trace_errors" "$link_packet" "$save_state" "$sgb_command"
        rows=$((rows + 1))
    done < "$temporary"
    if ((rows != ${#candidates[@]})); then
        echo "semantic reviewer returned $rows rows for ${#candidates[@]} files" >&2
        exit 1
    fi
} > "$REPORT.tmp"
mv "$REPORT.tmp" "$REPORT"
echo "semantic review written to $REPORT"
