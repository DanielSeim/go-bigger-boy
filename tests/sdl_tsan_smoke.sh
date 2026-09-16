#!/usr/bin/env bash
set -euo pipefail

# Exercise the interactive SDL event loop under ThreadSanitizer without
# requiring a physical display or input device. The window is driven through
# XTest via xdotool, while stdout/stderr and TSan reports are retained for
# diagnosis.
EXECUTABLE="${1:-./build-tsan/gbb}"
SMOKE_ROM="${GBB_SMOKE_ROM:-${2:-}}"
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
if [[ -n "$SMOKE_ROM" && ! -f "$SMOKE_ROM" ]]; then
    echo "smoke ROM does not exist: $SMOKE_ROM" >&2
    exit 2
fi
for command in timeout xvfb-run xdotool; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required GUI smoke command not found: $command" >&2
        exit 2
    fi
done

mkdir -p "$ARTIFACT_DIR"
export GBB_DEBUGGER_CAPTURE_DIR="${GBB_DEBUGGER_CAPTURE_DIR:-$ARTIFACT_DIR/debugger-captures}"
mkdir -p "$GBB_DEBUGGER_CAPTURE_DIR"

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
        rom=$3
        if [[ -n "$rom" ]]; then
            "$executable" "$rom" >"$output/stdout-stderr.log" 2>&1 &
        else
            "$executable" >"$output/stdout-stderr.log" 2>&1 &
        fi
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

        send_input() {
            if ! xdotool "$@" 2>>"$output/xdotool.log"; then
                echo "xdotool input failed: $*" >>"$output/xdotool.log"
            fi
        }
        click_at() {
            send_input mousemove --window "$1" "$2" "$3"
            send_input click --window "$1" 1
        }

        # A window manager is intentionally absent in Xvfb. Explicitly focus
        # the SDL window and send a harmless click so SDL accepts the targeted
        # keyboard event even when _NET_ACTIVE_WINDOW is unavailable.
        send_input windowfocus "$window"
        click_at "$window" 400 300
        # Xvfb does not provide a window manager on every runner. Targeted
        # xdotool events work without activation, so treat activation as a
        # best-effort convenience rather than a smoke-test prerequisite.
        send_input windowactivate --sync "$window"
        if [[ -z "$rom" ]]; then
            # Navigate the dashboard to its keyboard-shortcuts row and
            # activate it. These are real X11 input events, so dashboard
            # selection, focus, rendering, modal cleanup, and the SDL event
            # loop all run on the instrumented frontend thread.
            send_input key --window "$window" Down Down Down Enter
            sleep 0.3
            send_input key --window "$window" Escape
            sleep 0.3
        fi
        if [[ -n "$rom" ]]; then
            # Exercise the debugger at both the normal and compact supported
            # sizes. The same window geometry drives rendering and hit tests.
            debugger=""
            # The SDL window can be created before the ROM has finished
            # loading and before the debugger capability is available. Wait
            # for that startup boundary and retry the shortcut instead of
            # treating a missed first key event as a product failure.
            for attempt in $(seq 1 200); do
                debugger=$(xdotool search --name "Go Bigger Boy - Debugger" \
                    2>/dev/null | tail -n 1)
                [[ -n "$debugger" ]] && break
                if ((attempt == 1 || attempt % 5 == 0)); then
                    send_input windowfocus "$window"
                    send_input key --window "$window" --clearmodifiers F12
                fi
                sleep 0.1
            done
            if [[ -z "$debugger" ]]; then
                echo "debugger window did not appear" >&2
                kill -TERM "$pid" 2>/dev/null || true
                wait "$pid" || true
                exit 11
            fi
            send_input windowsize --sync "$debugger" 960 700
            # SDL applies the resize asynchronously. Give the debugger one
            # complete render turn at the compact size before changing modes;
            # otherwise slower runners can miss the normal 960x700 capture.
            sleep 1
            # F4 is a visible header button as well as a keyboard shortcut.
            # Use the shortcut here because SDL can receive it without a
            # window manager synthesizing a coordinate click.
            send_input key --window "$debugger" --clearmodifiers F4
            sleep 0.3
            # F1 opens the separate video viewer, whose three modes are also
            # reachable by visible buttons.
            click_at "$debugger" 320 30
            viewer=""
            for attempt in $(seq 1 100); do
                viewer=$(xdotool search --name "Go Bigger Boy - Video Viewers" \
                    2>/dev/null | tail -n 1)
                [[ -n "$viewer" ]] && break
                sleep 0.1
            done
            if [[ -z "$viewer" ]]; then
                echo "video viewer window did not appear" >&2
                kill -TERM "$pid" 2>/dev/null || true
                wait "$pid" || true
                exit 12
            fi
            click_at "$viewer" 260 66
            click_at "$viewer" 400 66
            send_input key --window "$viewer" F12
            sleep 0.3
            send_input mousemove --window "$debugger" 700 250
            send_input click --window "$debugger" 4
            send_input key --window "$debugger" Up
            send_input key --window "$debugger" F4
            sleep 0.3
            send_input key --window "$debugger" F12
        fi
        sleep 0.3
        send_input mousemove --window "$window" 320 240
        sleep 1
        kill -INT "$pid" 2>/dev/null || true
        child_status=0
        wait "$pid" || child_status=$?
        case "$child_status" in
            0|130|143) ;;
            *) exit "$child_status" ;;
        esac
    ' _ "$EXECUTABLE" "$ARTIFACT_DIR" "$SMOKE_ROM"
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
