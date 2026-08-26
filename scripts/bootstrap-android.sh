#!/usr/bin/env bash
set -euo pipefail

readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/.." && pwd)"
readonly sdk_platform="platforms;android-36"
readonly build_tools="build-tools;35.0.0"
readonly ndk="ndk;28.2.13676358"
readonly android_cmake="cmake;3.31.6"

find_android_sdk() {
    local candidate
    for candidate in "${ANDROID_HOME:-}" "${ANDROID_SDK_ROOT:-}" \
                     /usr/lib/android-sdk "${HOME}/Android/Sdk"; do
        if [[ -n "${candidate}" && -d "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

if ! command -v java >/dev/null 2>&1; then
    echo "JDK 17 is required but java was not found." >&2
    exit 1
fi

java_path="$(command -v java)"
if [[ "${java_path}" == /mnt/* ]]; then
    echo "Refusing to mix Windows Java (${java_path}) with a WSL build." >&2
    echo "Install OpenJDK 17 inside WSL and put it first in PATH." >&2
    exit 1
fi

java_major="$(java -XshowSettings:properties -version 2>&1 |
    sed -n 's/^[[:space:]]*java.version = \([0-9][0-9]*\).*/\1/p' | head -n 1)"
if [[ "${java_major}" != 17 ]]; then
    echo "JDK 17 is required; detected Java ${java_major:-unknown}." >&2
    exit 1
fi

if ! android_sdk="$(find_android_sdk)"; then
    echo "Android SDK not found. Set ANDROID_HOME to an Android SDK installation." >&2
    exit 1
fi

sdkmanager_path=""
if command -v sdkmanager >/dev/null 2>&1; then
    sdkmanager_path="$(command -v sdkmanager)"
else
    sdkmanager_path="$(find "${android_sdk}/cmdline-tools" -type f -name sdkmanager \
        2>/dev/null | sort -V | tail -n 1)"
fi
if [[ -z "${sdkmanager_path}" ]]; then
    echo "sdkmanager was not found. Install Android SDK command-line tools." >&2
    exit 1
fi

export ANDROID_HOME="${android_sdk}"
export ANDROID_SDK_ROOT="${android_sdk}"

"${sdkmanager_path}" "${sdk_platform}" "${build_tools}" "${ndk}" "${android_cmake}"
"${repository_root}/android/fetch-sdl.sh"

echo "Android toolchain is ready at ${android_sdk}."
echo "Build with: scripts/build-android.sh debug"
