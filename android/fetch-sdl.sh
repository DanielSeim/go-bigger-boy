#!/usr/bin/env bash
set -euo pipefail

sdl_version=3.4.2
archive="SDL3-devel-${sdl_version}-android.zip"
expected_sha256=7266be52ecebd1ddc8f1d35df87e1ff7e555a767e5d73409627975bc1548a07a
script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
destination="${script_directory}/app/libs"
temporary_directory="$(mktemp -d)"
trap 'rm -rf -- "${temporary_directory}"' EXIT

curl --fail --location --retry 3 \
    "https://github.com/libsdl-org/SDL/releases/download/release-${sdl_version}/${archive}" \
    --output "${temporary_directory}/${archive}"
echo "${expected_sha256}  ${temporary_directory}/${archive}" | sha256sum --check -
mkdir -p "${destination}"
unzip -j -o "${temporary_directory}/${archive}" \
    "SDL3-${sdl_version}.aar" -d "${destination}"
