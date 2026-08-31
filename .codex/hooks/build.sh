#!/usr/bin/env bash
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="${PRINTERCTL_BUILD_DIR:-$ROOT/build}"
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then cmake -S "$ROOT" -B "$BUILD_DIR"; fi
cmake --build "$BUILD_DIR" --parallel
