#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$ROOT_DIR/cpp"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

if [[ ! -d "$CPP_DIR" ]]; then
    echo "Error: cpp directory not found in $ROOT_DIR" >&2
    exit 1
fi

cmake -S "$CPP_DIR" -B "$BUILD_DIR" "$@"
cmake --build "$BUILD_DIR" --config Release

echo "Build artifacts available in $BUILD_DIR"