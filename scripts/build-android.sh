#!/usr/bin/env bash
set -euo pipefail

readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/.." && pwd)"
readonly android_directory="${repository_root}/android"
readonly variant="${1:-debug}"

# A Windows Gradle invocation can populate ~/.gradle with native libraries that
# are unusable from WSL. Keep the wrapper cache repository-local and ignored so
# the two operating systems never share native Gradle state by accident.
export GRADLE_USER_HOME="${GBB_GRADLE_USER_HOME:-${repository_root}/.cache/gradle}"

case "${variant}" in
    debug)
        tasks=(assembleDebug)
        output="${android_directory}/app/build/outputs/apk/debug/app-debug.apk"
        ;;
    release)
        for variable in GBB_ANDROID_KEYSTORE_FILE \
                        GBB_ANDROID_KEYSTORE_PASSWORD \
                        GBB_ANDROID_KEY_PASSWORD; do
            if [[ -z "${!variable:-}" ]]; then
                echo "Release build requires ${variable}." >&2
                exit 1
            fi
        done
        if [[ ! -f "${GBB_ANDROID_KEYSTORE_FILE}" ]]; then
            echo "Release keystore not found: ${GBB_ANDROID_KEYSTORE_FILE}" >&2
            exit 1
        fi
        tasks=(assembleRelease bundleRelease)
        output="${android_directory}/app/build/outputs/apk/release/app-release.apk"
        ;;
    *)
        echo "Usage: $0 [debug|release]" >&2
        exit 2
        ;;
esac

java_path="$(command -v java 2>/dev/null || true)"
if [[ -z "${java_path}" ]]; then
    echo "JDK 17 is required. Run scripts/bootstrap-android.sh first." >&2
    exit 1
fi
if [[ "${java_path}" == /mnt/* ]]; then
    echo "Refusing to mix Windows Java (${java_path}) with a WSL build." >&2
    exit 1
fi

if [[ -z "${ANDROID_HOME:-}" && -z "${ANDROID_SDK_ROOT:-}" ]]; then
    if [[ -d /usr/lib/android-sdk ]]; then
        export ANDROID_HOME=/usr/lib/android-sdk
        export ANDROID_SDK_ROOT=/usr/lib/android-sdk
    elif [[ -d "${HOME}/Android/Sdk" ]]; then
        export ANDROID_HOME="${HOME}/Android/Sdk"
        export ANDROID_SDK_ROOT="${HOME}/Android/Sdk"
    else
        echo "Android SDK not found. Run scripts/bootstrap-android.sh first." >&2
        exit 1
    fi
fi

if [[ ! -s "${android_directory}/app/libs/SDL3-3.4.2.aar" ]]; then
    "${android_directory}/fetch-sdl.sh"
fi

(
    cd "${android_directory}"
    ./gradlew --no-daemon "${tasks[@]}"
)

echo "Android ${variant} build completed: ${output}"
