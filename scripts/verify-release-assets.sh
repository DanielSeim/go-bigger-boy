#!/usr/bin/env bash
set -euo pipefail

release_root="${1:-}"
if [[ -z "$release_root" || ! -d "$release_root" ]]; then
    echo "Usage: $0 <downloaded-release-assets>" >&2
    exit 2
fi

find_asset() {
    local name="$1"
    find "$release_root" -type f -name "$name" -print -quit
}

require_asset() {
    local name="$1"
    local path
    path="$(find_asset "$name")"
    if [[ -z "$path" || ! -s "$path" ]]; then
        echo "Required release asset is missing or empty: $name" >&2
        exit 1
    fi
    printf '%s\n' "$path"
}

require_archive_member() {
    local archive="$1"
    local member="$2"
    # Consume the complete listing so pipefail does not turn grep's early
    # success into an unzip SIGPIPE failure on large archives.
    if ! unzip -Z1 "$archive" | grep -Fx "$member" >/dev/null; then
        echo "Archive is missing required member: $(basename "$archive")/$member" >&2
        exit 1
    fi
}

echo "Checking desktop release assets..."
linux_appimage="$(require_asset go-bigger-boy-linux-x64.AppImage)"
windows_archive="$(require_asset go-bigger-boy-windows-x64.zip)"
macos_arm_archive="$(require_asset go-bigger-boy-macos-arm64.tar.gz)"
macos_x64_archive="$(require_asset go-bigger-boy-macos-x64.tar.gz)"

chmod +x "$linux_appimage"
version_output="$(APPIMAGE_EXTRACT_AND_RUN=1 "$linux_appimage" --version)"
if [[ ! "$version_output" =~ ^Go\ Bigger\ Boy\ [0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "AppImage --version returned an unexpected result: $version_output" >&2
    exit 1
fi

require_archive_member "$windows_archive" gbb.exe
require_archive_member "$windows_archive" gbb-updater.exe
require_archive_member "$windows_archive" SDL3.dll
require_archive_member "$windows_archive" settings.ini

for macos_archive in "$macos_arm_archive" "$macos_x64_archive"; do
    if ! tar -tzf "$macos_archive" | grep -Fx './Go Bigger Boy.app/Contents/MacOS/Go Bigger Boy' >/dev/null \
       && ! tar -tzf "$macos_archive" | grep -Fx 'Go Bigger Boy.app/Contents/MacOS/Go Bigger Boy' >/dev/null; then
        echo "macOS archive is missing the application executable: $(basename "$macos_archive")" >&2
        exit 1
    fi
done

echo "Checking Android release assets..."
android_apk="$(require_asset go-bigger-boy-android.apk)"
android_aab="$(require_asset go-bigger-boy-android.aab)"
require_archive_member "$android_apk" AndroidManifest.xml
require_archive_member "$android_aab" base/manifest/AndroidManifest.xml

echo "Checking Web release assets..."
require_asset index.html >/dev/null
require_asset index.js >/dev/null
require_asset index.wasm >/dev/null
require_asset go_bigger_boy_logo.png >/dev/null

echo "Release asset preflight passed."
