#!/usr/bin/env bash
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
BUILD_DIR="${PRINTERCTL_BUILD_DIR:-$ROOT/build}"
[[ -f "$BUILD_DIR/CTestTestfile.cmake" ]] || { echo "CTest metadata not found; run build hook first" >&2; exit 2; }
ctest --test-dir "$BUILD_DIR" --output-on-failure
