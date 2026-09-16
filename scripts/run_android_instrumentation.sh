#!/usr/bin/env bash
set -u -o pipefail

readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/.." && pwd)"
readonly diagnostics_directory="${repository_root}/android-ci-diagnostics"

mkdir -p "${diagnostics_directory}"
test_status=1
for attempt in 1 2; do
    (cd "${repository_root}/android" && \
        ./gradlew --no-daemon --stacktrace connectedDebugAndroidTest) \
        2>&1 | tee "${diagnostics_directory}/instrumentation-attempt-${attempt}.log"
    test_status=${PIPESTATUS[0]}
    if [[ "${test_status}" -eq 0 ]]; then
        break
    fi
    if [[ "${attempt}" -lt 2 ]]; then
        echo "Instrumentation attempt ${attempt} failed; retrying once." >&2
        sleep 10
    fi
done

timeout 10s adb devices -l > "${diagnostics_directory}/adb-devices.txt" 2>&1 || true
timeout 15s adb logcat -d -v threadtime > "${diagnostics_directory}/logcat.txt" 2>&1 || true
exit "${test_status}"
