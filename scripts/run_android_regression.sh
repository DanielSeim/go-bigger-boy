#!/usr/bin/env bash
set -euo pipefail

readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/.." && pwd)"
readonly android_directory="${repository_root}/android"
readonly native_build_directory="${GBB_ANDROID_REGRESSION_BUILD_DIR:-${repository_root}/build-android-regression}"
readonly output_directory="${GBB_ANDROID_REGRESSION_OUTPUT_DIR:-${TMPDIR:-/tmp}/gbb-android-regression}"
readonly package_name="com.danielseim.gbb.debug"
readonly library_activity="com.danielseim.gbb.LibraryActivity"

# Keep Gradle's native cache repository-local so a WSL run cannot reuse
# incompatible native libraries produced by a Windows Gradle installation.
export GRADLE_USER_HOME="${GBB_GRADLE_USER_HOME:-${repository_root}/.cache/gradle}"

if [[ -z "${ANDROID_HOME:-}" && -z "${ANDROID_SDK_ROOT:-}" ]]; then
    if [[ -d /usr/lib/android-sdk ]]; then
        export ANDROID_HOME=/usr/lib/android-sdk
        export ANDROID_SDK_ROOT=/usr/lib/android-sdk
    elif [[ -d "${HOME}/Android/Sdk" ]]; then
        export ANDROID_HOME="${HOME}/Android/Sdk"
        export ANDROID_SDK_ROOT="${HOME}/Android/Sdk"
    elif [[ -d /home/danie/android-sdk ]]; then
        export ANDROID_HOME=/home/danie/android-sdk
        export ANDROID_SDK_ROOT=/home/danie/android-sdk
    else
        echo "Android SDK not found. Run scripts/bootstrap-android.sh first." >&2
        exit 1
    fi
fi

readonly adb_binary="${ADB:-${ANDROID_HOME:-${ANDROID_SDK_ROOT}}/platform-tools/adb}"
readonly apk="${android_directory}/app/build/outputs/apk/debug/app-debug.apk"

native_targets=(
    gameboy_frontend_contract_tests
    gameboy_android_touch_layout_contract_tests
    gameboy_ppu_sgb_contract_tests
    gameboy_apu_cycle_contract_tests
    gameboy_sdl_frame_pacer_tests
)

cmake -S "${repository_root}" -B "${native_build_directory}" \
    -DGAMEBOY_BUILD_TESTS=ON -DGAMEBOY_BUILD_SDL=ON
cmake --build "${native_build_directory}" --target "${native_targets[@]}" \
    --parallel
ctest --test-dir "${native_build_directory}" --output-on-failure \
    -R 'gameboy_(frontend_contract|android_touch_layout_contract|ppu_sgb_contract|apu_cycle_contract|sdl_frame_pacer)$'

if [[ ! -s "${android_directory}/app/libs/SDL3-3.4.2.aar" ]]; then
    "${android_directory}/fetch-sdl.sh"
fi

(
    cd "${android_directory}"
    ./gradlew --no-daemon test
)
"${script_directory}/build-android.sh" debug

if [[ "${GBB_SKIP_DEVICE_TESTS:-0}" == "1" ]]; then
    echo "Connected-device checks skipped (GBB_SKIP_DEVICE_TESTS=1)."
    exit 0
fi

if [[ ! -x "${adb_binary}" ]]; then
    echo "Connected-device checks skipped: ADB not found at ${adb_binary}."
    exit 0
fi

mapfile -t devices < <("${adb_binary}" devices | awk '$2 == "device" {print $1}')
if (( ${#devices[@]} == 0 )); then
    echo "Connected-device checks skipped: no authorized ADB device found."
    exit 0
fi
if (( ${#devices[@]} > 1 )) && [[ -z "${ADB_SERIAL:-}" ]]; then
    echo "More than one ADB device is connected; set ADB_SERIAL to choose one." >&2
    printf '  %s\n' "${devices[@]}" >&2
    exit 1
fi
readonly device_serial="${ADB_SERIAL:-${devices[0]}}"
adb() { "${adb_binary}" -s "${device_serial}" "$@"; }

echo "Running Android instrumentation on ${device_serial}..."
(
    cd "${android_directory}"
    ANDROID_SERIAL="${device_serial}" ./gradlew --no-daemon connectedDebugAndroidTest
)

mkdir -p "${output_directory}"
adb install -r "${apk}" >/dev/null
adb shell am force-stop "${package_name}"
adb shell am start -W -n "${package_name}/${library_activity}" \
    --ez "com.danielseim.gbb.SKIP_UPDATE_CHECK" true >/dev/null
adb shell dumpsys activity activities | grep -q \
    "${package_name}/${library_activity}"
adb exec-out screencap -p > "${output_directory}/library.png"

echo "Android regression checks passed. Device screenshot: ${output_directory}/library.png"
