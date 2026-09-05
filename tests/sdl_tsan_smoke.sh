#!/usr/bin/env bash
set -euo pipefail

# Exercise the interactive SDL event loop under ThreadSanitizer without
# requiring a physical display or input device. The window is driven through
# XTest via xdotool, while stdout/stderr and TSan reports are retained for
# diagnosis.
EXECUTABLE="${1:-./build-tsan/gbb}"
DURATION_SECONDS="${GUI_TSAN_DURATION_SECONDS:-20}"
ARTIFACT_DIR="${GUI_TSAN_ARTIFACT_DIR:-tsan-gui-artifacts}"

if [[ ! -x "$EXECUTABLE" ]]; then
    echo "SDL executable not found or not executable: $EXECUTABLE" >&2
    exit 2
fi
if ! [[ "$DURATION_SECONDS" =~ ^[0-9]+$ ]] || ((DURATION_SECONDS < 5)); then
    echo "GUI_TSAN_DURATION_SECONDS must be an integer of at least 5" >&2
    exit 2
fi
for command in timeout xvfb-run xdotool; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required GUI smoke command not found: $command" >&2
        exit 2
    fi
done

mkdir -p "$ARTIFACT_DIR"

tsan_options="${TSAN_OPTIONS:-halt_on_error=1:second_deadlock_stack=1:history_size=7}"
if [[ "$tsan_options" != *log_path=* ]]; then
    tsan_options="${tsan_options}:log_path=$(realpath -m "$ARTIFACT_DIR/tsan")"
fi

set +e
TSAN_OPTIONS="$tsan_options" timeout --signal=INT --kill-after=5s \
    --preserve-status "${DURATION_SECONDS}s" xvfb-run -a \
    -s "-screen 0 1024x768x24" bash -c '
        set -eu
        executable=$1
        output=$2
        "$executable" >"$output/stdout-stderr.log" 2>&1 &
        pid=$!
        window=""
        for attempt in $(seq 1 100); do
            window=$(xdotool search --name "Go Bigger Boy" 2>/dev/null | tail -n 1)
            [[ -n "$window" ]] && break
            sleep 0.1
        done
        if [[ -z "$window" ]]; then
            echo "SDL window did not appear" >&2
            kill -TERM "$pid" 2>/dev/null
            wait "$pid" || true
            exit 10
        fi

        # Xvfb does not provide a window manager on every runner. Targeted
        # xdotool events work without activation, so treat activation as a
        # best-effort convenience rather than a smoke-test prerequisite.
        xdotool windowactivate --sync "$window" 2>/dev/null || true
        # Navigate the dashboard and open/close the help modal. These are real
        # X11 input events, so event dispatch, focus, rendering, and modal
        # cleanup all run on the instrumented SDL thread.
        xdotool key --window "$window" Down Up
        xdotool key --window "$window" F1
        sleep 0.3
        xdotool key Escape
        sleep 0.3
        xdotool mousemove --window "$window" 320 240
        sleep 1
        kill -INT "$pid" 2>/dev/null || true
        child_status=0
        wait "$pid" || child_status=$?
        case "$child_status" in
            0|130|143) ;;
            *) exit "$child_status" ;;
        esac
    ' _ "$EXECUTABLE" "$ARTIFACT_DIR"
status=$?
set -e

case "$status" in
    0|124|130|143) ;;
    *)
        echo "interactive SDL TSan smoke failed with status $status" >&2
        exit "$status"
        ;;
esac

echo "interactive SDL TSan smoke completed (status $status)"
