#!/usr/bin/env bash
set -euo pipefail

appdir="${1:-package}"
output="${2:-go-bigger-boy-linux-x64.AppImage}"
linuxdeploy_version="${LINUXDEPLOY_VERSION:-1-alpha-20251107-1}"
tool_path="${LINUXDEPLOY_PATH:-${TMPDIR:-/tmp}/gbb-linuxdeploy-${linuxdeploy_version}.AppImage}"
tool_url="https://github.com/linuxdeploy/linuxdeploy/releases/download/${linuxdeploy_version}/linuxdeploy-x86_64.AppImage"

if [[ ! -x "$tool_path" ]]; then
    curl --fail --location --retry 5 --retry-all-errors --retry-delay 2 \
        --connect-timeout 20 --max-time 300 "$tool_url" \
        --output "$tool_path"
    chmod +x "$tool_path"
fi

test -x "$appdir/usr/bin/gbb"
test -f "$appdir/usr/share/applications/go-bigger-boy.desktop"
test -f "$appdir/usr/share/icons/hicolor/512x512/apps/go-bigger-boy.png"

export LDAI_OUTPUT="$output"
export APPIMAGE_EXTRACT_AND_RUN=1
"$tool_path" --appdir "$appdir" \
    -e "$appdir/usr/bin/gbb" \
    -d "$appdir/usr/share/applications/go-bigger-boy.desktop" \
    -i "$appdir/usr/share/icons/hicolor/512x512/apps/go-bigger-boy.png" \
    --output appimage

if [[ "$output" == */* ]]; then
    output_command="$output"
else
    output_command="./$output"
fi
test -x "$output_command"
APPIMAGE_EXTRACT_AND_RUN=1 "$output_command" --version
