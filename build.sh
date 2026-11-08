#!/usr/bin/env bash
# Build with the toolchain installed by the VS Code Raspberry Pi Pico extension.
set -euo pipefail
cd "$(dirname "$0")"
SDK_ROOT="${HOME}/.pico-sdk"
export PICO_SDK_PATH="${PICO_SDK_PATH:-${SDK_ROOT}/sdk/2.2.0}"
export PICO_TOOLCHAIN_PATH="${PICO_TOOLCHAIN_PATH:-${SDK_ROOT}/toolchain/14_2_Rel1}"
export PICO_EXTRAS_PATH="${PICO_EXTRAS_PATH:-$(pwd)/lib/pico-extras}"
CMAKE="${SDK_ROOT}/cmake/v3.31.5/bin/cmake"; [ -x "$CMAKE" ] || CMAKE=cmake
NINJA="${SDK_ROOT}/ninja/v1.12.1/ninja";     [ -x "$NINJA" ] || NINJA=ninja
PICOTOOL_DIR="${SDK_ROOT}/picotool/2.2.0/picotool"
[ -d lib/pico-extras ] || { echo "lib/pico-extras missing: git clone --depth 1 https://github.com/raspberrypi/pico-extras lib/pico-extras"; exit 1; }
"$CMAKE" -S . -B build -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DCMAKE_MAKE_PROGRAM="$(command -v "$NINJA" || echo "$NINJA")" \
    -Dpicotool_DIR="$PICOTOOL_DIR" "$@"
"$NINJA" -C build
ls -la build/picocarpod.uf2
