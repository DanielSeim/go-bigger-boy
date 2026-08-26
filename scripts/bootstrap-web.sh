#!/usr/bin/env bash
set -euo pipefail

readonly script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repository_root="$(cd -- "${script_directory}/.." && pwd)"
readonly toolchain_root="${repository_root}/.cache/toolchains"
readonly emsdk_directory="${toolchain_root}/emsdk"
readonly sdl_source_directory="${toolchain_root}/SDL-3.4.2"
readonly sdl_build_directory="${toolchain_root}/SDL-3.4.2-build-web"
readonly sdl_install_directory="${toolchain_root}/sdl3-emscripten"
readonly emsdk_version=4.0.15
readonly sdl_tag=release-3.4.2

for tool in git cmake; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Required tool not found: ${tool}" >&2
        exit 1
    fi
done

mkdir -p "${toolchain_root}"
if [[ ! -d "${emsdk_directory}/.git" ]]; then
    git clone --depth 1 https://github.com/emscripten-core/emsdk.git \
        "${emsdk_directory}"
elif [[ ! -x "${emsdk_directory}/emsdk" ]]; then
    rm -f -- "${emsdk_directory}/.git/shallow.lock"
    git -C "${emsdk_directory}" fetch --depth 1 origin main
    git -C "${emsdk_directory}" checkout --detach FETCH_HEAD
fi

"${emsdk_directory}/emsdk" install "${emsdk_version}"
"${emsdk_directory}/emsdk" activate "${emsdk_version}"
# shellcheck disable=SC1091
source "${emsdk_directory}/emsdk_env.sh"

if [[ ! -d "${sdl_source_directory}/.git" ]]; then
    git clone --depth 1 --branch "${sdl_tag}" \
        https://github.com/libsdl-org/SDL.git "${sdl_source_directory}"
elif [[ ! -f "${sdl_source_directory}/CMakeLists.txt" ]]; then
    # Recover cleanly if an earlier shallow clone was interrupted.
    rm -f -- "${sdl_source_directory}/.git/shallow.lock"
    git -C "${sdl_source_directory}" fetch --depth 1 origin "${sdl_tag}"
    git -C "${sdl_source_directory}" checkout --detach FETCH_HEAD
fi

emcmake cmake -S "${sdl_source_directory}" -B "${sdl_build_directory}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF \
    -DSDL_STATIC=ON \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX="${sdl_install_directory}"
cmake --build "${sdl_build_directory}" --parallel
cmake --install "${sdl_build_directory}"

echo "Web toolchain is ready under ${toolchain_root}."
echo "Build with: scripts/build-web.sh"
