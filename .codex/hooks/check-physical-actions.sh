#!/usr/bin/env bash
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
BASE_REF="${PRINTERCTL_BASE_REF:-HEAD~1}"
git rev-parse --verify "$BASE_REF" >/dev/null 2>&1 || { echo "Base ref $BASE_REF not found; skipping" >&2; exit 0; }
PATTERN='(^|[^A-Za-z0-9_])(G28|G0|G1|M104|M109|M140|M190|M112|M410)([^A-Za-z0-9_]|$)'
matches="$(git diff --unified=0 "$BASE_REF"...HEAD -- '*.c' '*.h' '*.md' | grep '^+' | grep -Ev '^\+\+\+' | grep -E "$PATTERN" || true)"
if [[ -n "$matches" ]]; then echo 'Physical-action tokens added:'; echo "$matches"; echo 'Manual safety review required.'; exit 3; fi
echo 'Physical-action diff scan: PASS'
