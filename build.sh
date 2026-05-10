#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build/f133"
TOOLCHAIN_FILE="${SCRIPT_DIR}/cmake/toolchains/f133_toolchain.cmake"
JOBS="${JOBS:-$(nproc)}"

if ! command -v cmake >/dev/null 2>&1; then
    echo "error: cmake not found" >&2
    exit 1
fi

if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    echo "error: missing toolchain file: ${TOOLCHAIN_FILE}" >&2
    exit 1
fi

cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"

echo "build output: ${SCRIPT_DIR}/bin/main"
