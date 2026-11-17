#!/usr/bin/env bash
# Log assertions for a headless editor run (see tools/run_headless.sh).
#
# Usage: check_log.sh LOG CATEGORY_REGEX REQUIRED_REGEX [REQUIRED_REGEX...]
#   Fails unless every REQUIRED_REGEX matches a line of LOG, and when LOG has a line of a category matching
#   CATEGORY_REGEX that matches none of the REQUIRED_REGEX (a failure of the action is logged by the same category:
#   "Failed to ...", "template missing", "already exists"). Lines look like: [12:00:00.000] [info] [Editor/NewLevel] text
set -euo pipefail

log="${1:?log}"; category="${2:?category regex}"; shift 2
[[ $# -gt 0 ]] || { echo "usage: check_log.sh LOG CATEGORY_REGEX REQUIRED_REGEX..." >&2; exit 2; }
[[ -f "$log" ]] || { echo "FAIL: no log: $log" >&2; exit 1; }
fail=0

for required in "$@"; do
	if grep -Eq -- "$required" "$log"; then
		echo "ok: log has a line matching: $required"
	else
		echo "::error::expected log line not found: $required (log: $log)"
		fail=1
	fi
done

unexpected=$(grep -E -- "\[($category)\]" "$log" || true)
for required in "$@"; do
	unexpected=$(grep -Ev -- "$required" <<<"$unexpected" || true)
done
if [[ -n "$unexpected" ]]; then
	echo "::error::unexpected lines of category $category in $log:"
	echo "$unexpected" | head -20
	fail=1
fi

exit "$fail"
