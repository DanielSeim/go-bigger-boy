#!/usr/bin/env bash
set -euo pipefail

# Validate the reviewed corpus before a local or CI campaign. This deliberately
# checks only deterministic hygiene; semantic review of newly generated inputs
# still happens before promote_corpus.sh --approve.
CORPUS_DIR="${1:-tests/fuzz/corpus}"
MAX_LENGTH="${FUZZ_MAX_LEN:-2097152}"

if [[ ! -d "$CORPUS_DIR" ]]; then
    echo "corpus directory not found: $CORPUS_DIR" >&2
    exit 2
fi
if ! [[ "$MAX_LENGTH" =~ ^[0-9]+$ ]] || ((MAX_LENGTH < 1)); then
    echo "FUZZ_MAX_LEN must be a positive integer" >&2
    exit 2
fi

mapfile -d '' seeds < <(find "$CORPUS_DIR" -maxdepth 1 -type f \
    -name '*.seed' -print0 | sort -z)
if (( ${#seeds[@]} == 0 )); then
    echo "corpus contains no .seed files: $CORPUS_DIR" >&2
    exit 1
fi

declare -A hashes
total_bytes=0
for path in "${seeds[@]}"; do
    size=$(stat -c '%s' -- "$path")
    if ((size == 0)); then
        echo "empty corpus seed: $path" >&2
        exit 1
    fi
    if ((size > MAX_LENGTH)); then
        echo "corpus seed exceeds FUZZ_MAX_LEN ($MAX_LENGTH): $path" >&2
        exit 1
    fi
    hash=$(sha256sum -- "$path" | awk '{print $1}')
    if [[ -n "${hashes[$hash]:-}" ]]; then
        echo "duplicate corpus seed content: $path and ${hashes[$hash]}" >&2
        exit 1
    fi
    hashes[$hash]=$path
    total_bytes=$((total_bytes + size))
done

manifest="$CORPUS_DIR/MANIFEST.sha256"
if [[ ! -f "$manifest" ]]; then
    echo "corpus manifest missing: $manifest" >&2
    exit 1
fi
if ! (cd "$CORPUS_DIR" && sha256sum --strict --quiet --check MANIFEST.sha256); then
    echo "corpus manifest does not match reviewed seeds: $manifest" >&2
    exit 1
fi
manifest_count=$(awk 'NF == 2 && $2 ~ /\.seed$/ {count++} END {print count + 0}' \
    "$manifest")
if ((manifest_count != ${#seeds[@]})); then
    echo "corpus manifest seed count differs from directory" >&2
    exit 1
fi

printf 'corpus check passed: %d seeds, %d bytes, max %d bytes\n' \
    "${#seeds[@]}" "$total_bytes" "$MAX_LENGTH"
