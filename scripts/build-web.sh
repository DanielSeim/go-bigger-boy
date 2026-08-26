#!/usr/bin/env bash
set -euo pipefail

readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/.." && pwd)"
readonly local_toolchain_root="${repository_root}/.cache/toolchains"
readonly build_directory="${repository_root}/build-web"
readonly serve="${1:-}"

if [[ -n "${serve}" && "${serve}" != --serve ]]; then
    echo "Usage: $0 [--serve]" >&2
    exit 2
fi

if [[ -z "${EMSDK:-}" ]]; then
    emsdk_environment="${local_toolchain_root}/emsdk/emsdk_env.sh"
    if [[ -f "${emsdk_environment}" ]]; then
        # shellcheck disable=SC1090
        source "${emsdk_environment}"
    elif ! command -v emcmake >/dev/null 2>&1; then
        echo "Emscripten is not active. Run scripts/bootstrap-web.sh first." >&2
        exit 1
    fi
elif ! command -v emcmake >/dev/null 2>&1; then
    echo "EMSDK is set but emcmake was not found in PATH." >&2
    echo "Source emsdk_env.sh or unset EMSDK to use the repository bootstrap." >&2
    exit 1
fi

sdl3_directory="${SDL3_DIR:-${local_toolchain_root}/sdl3-emscripten/lib/cmake/SDL3}"
if [[ ! -f "${sdl3_directory}/SDL3Config.cmake" ]]; then
    echo "Emscripten SDL3 was not found at ${sdl3_directory}." >&2
    echo "Run scripts/bootstrap-web.sh or set SDL3_DIR." >&2
    exit 1
fi

emcmake cmake --fresh -S "${repository_root}" -B "${build_directory}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DGAMEBOY_BUILD_TESTS=OFF \
    -DGAMEBOY_BUILD_SDL=ON \
    -DSDL3_DIR="${sdl3_directory}"
cmake --build "${build_directory}" --target gameboy_web --parallel

for artifact in index.html index.js index.wasm; do
    test -s "${build_directory}/web/${artifact}"
done

echo "Web build completed: ${build_directory}/web/index.html"
if [[ "${serve}" == --serve ]]; then
    exec emrun "${build_directory}/web/index.html"
fi
