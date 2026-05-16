#!/usr/bin/env bash
# scripts/check-status-fresh.sh
#
# Enforces that docs/STATUS.md's "Last completed history" pointer matches the
# newest file under docs/histories/. A stale STATUS.md means a new session
# orienting on it will believe the project is older than it is.
#
# Failure modes:
#   - STATUS.md does not exist (caught upstream by check-docs.sh).
#   - STATUS.md does not link to any file under docs/histories/.
#   - STATUS.md's linked history is not the newest one by mtime-sorted
#     basename under docs/histories/.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
status_file="${repo_root}/docs/STATUS.md"

if [[ ! -f "${status_file}" ]]; then
  echo "STATUS.md missing: ${status_file}"
  exit 1
fi

# Newest history file (basenames sort lexicographically because they are
# YYYYMMDD-HHmm-prefixed).
newest_history="$(find "${repo_root}/docs/histories" \
  -type f -name '*.md' \
  ! -name 'template.md' \
  -printf '%f\n' |
  sort |
  tail -1)"

if [[ -z "${newest_history}" ]]; then
  echo "no history entries found under docs/histories/"
  exit 1
fi

# Find the basename of the history file STATUS.md points at. We accept any
# link to a file matching the YYYYMMDD-HHmm-*.md pattern inside histories/.
linked_history="$(grep -oE 'histories/[0-9]{4}-[0-9]{2}/[0-9]{8}-[0-9]{4}-[a-z0-9-]+\.md' \
  "${status_file}" |
  sed -E 's|.*/||' |
  sort -u |
  tail -1)"

if [[ -z "${linked_history}" ]]; then
  echo "STATUS.md does not link to any history file under docs/histories/"
  echo "expected a reference like docs/histories/YYYY-MM/YYYYMMDD-HHmm-<slug>.md"
  exit 1
fi

if [[ "${linked_history}" != "${newest_history}" ]]; then
  echo "STATUS.md is stale"
  echo "  STATUS.md points at:        ${linked_history}"
  echo "  newest history on disk:     ${newest_history}"
  echo "update docs/STATUS.md in the same commit that lands the newer history"
  exit 1
fi

echo "STATUS.md freshness check passed (latest history: ${linked_history})"
