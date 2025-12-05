#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$ROOT_DIR/cpp"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"

cmake -S "$CPP_DIR" -B "$BUILD_DIR"
cmake --build "$BUILD_DIR" --config Release

BIN_CANDIDATES=(
  "$BUILD_DIR/shortcut_router"
  "$BUILD_DIR/Release/shortcut_router"
  "$BUILD_DIR/Debug/shortcut_router"
)

BINARY=""
for candidate in "${BIN_CANDIDATES[@]}"; do
  if [[ -x "$candidate" ]]; then
    BINARY="$candidate"
    break
  fi
done

if [[ -z "$BINARY" ]]; then
  echo "Error: shortcut_router binary not found after build." >&2
  exit 1
fi

"$BINARY" "$@"
