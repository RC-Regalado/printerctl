#!/usr/bin/env bash
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
failed=0
check(){ local path="$1" pattern="$2" label="$3"; [[ -e "$ROOT/$path" ]] || return 0; local out; out="$(grep -RInE --exclude-dir=.git --exclude='*.md' --exclude='*.txt' "$pattern" "$ROOT/$path" || true)"; if [[ -n "$out" ]]; then echo "[BOUNDARY] $label"; echo "$out"; failed=1; fi; }
check src/protocol 'print_engine|printer_|serial_|marlin_|AF_UNIX|SOCK_STREAM|socket[[:space:]]*\(|bind[[:space:]]*\(|listen[[:space:]]*\(|accept[[:space:]]*\(' 'protocol must stay independent of printer/core/serial/Marlin/socket transport'
check src/core 'AF_UNIX|SOCK_STREAM|PLP|plp1|socket[[:space:]]*\(|bind[[:space:]]*\(|listen[[:space:]]*\(|accept[[:space:]]*\(' 'core must remain unaware of daemon/socket/PLP'
check src/serial 'printerctld|daemon_|PLP|plp1' 'serial must not depend on daemon/PLP'
[[ "$failed" -eq 0 ]] || { echo 'Architecture boundary violations detected.' >&2; exit 1; }
echo 'Architecture boundary scan: PASS'
