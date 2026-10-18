#!/usr/bin/env bash
# Sanity checks for a headless editor run (see tools/run_headless.sh).
#
# Usage: check_smoke.sh SCREENSHOT LOG [GEOMETRY [MIN_STDDEV [REFERENCE]]]
#   GEOMETRY    ImageMagick crop of the viewport centre   (default: 200x200+860+450)
#   MIN_STDDEV  minimum normalized pixel standard deviation of that crop (default: 0.055)
#   REFERENCE   reference PNG (tools/headless/reference: a downscaled llvmpipe screenshot); the screenshot is
#               scaled to its size and compared. REPORT-ONLY for now: the metric is printed, the exit code is
#               not affected. SNK_SMOKE_FUZZ (per-pixel colour tolerance, default 4%) and SNK_SMOKE_MAX_DIFF_PCT
#               (share of differing pixels that is flagged, default 2) tune the comparison.
#
# Fails when the log has error/assert/exception lines, or when the crop has less texture than bare sky/water,
# i.e. the model (the robot on smoke_level) was not drawn. Needs ImageMagick (convert).
set -euo pipefail

shot="${1:?screenshot}"; log="${2:?log}"
geom="${3:-200x200+860+450}"
min_std="${4:-0.055}"
fail=0

[[ -s "$shot" ]] || { echo "FAIL: no screenshot: $shot" >&2; exit 1; }
[[ -f "$log" ]] || { echo "FAIL: no log: $log" >&2; exit 1; }

if bad=$(grep -inE '\[(error|critical)\]|assert|exception' "$log"); then
	echo "FAIL: suspicious log lines in $log:" >&2
	echo "$bad" | head -20 >&2
	fail=1
fi

std=$(convert "$shot" -crop "$geom" +repage -colorspace Gray -format '%[fx:standard_deviation]' info:)
echo "viewport crop $geom stddev=$std (min $min_std)"
if ! awk -v s="$std" -v m="$min_std" 'BEGIN { exit !(s >= m) }'; then
	echo "FAIL: viewport centre looks empty (stddev $std < $min_std)" >&2
	fail=1
fi

ref="${5:-}"
compare_reference() {
	local ref="$1" fuzz="${SNK_SMOKE_FUZZ:-4%}" max_pct="${SNK_SMOKE_MAX_DIFF_PCT:-2}"
	if [[ ! -s "$ref" ]]; then
		echo "reference diff: no reference $ref (skipped)"
		return 0
	fi
	local size rw rh diff_px pct verdict=ok
	size=$(identify -format '%w %h' "$ref") || { echo "reference diff: cannot read $ref"; return 0; }
	read -r rw rh <<<"$size"
	diff_px=$(convert "$shot" -resize "${rw}x${rh}!" png:- | compare -metric AE -fuzz "$fuzz" "$ref" png:- null: 2>&1 || true)
	diff_px="${diff_px%% *}"
	if [[ "$diff_px" =~ ^[0-9]+$ && "${rw:-}" =~ ^[0-9]+$ && "${rh:-}" =~ ^[0-9]+$ ]]; then
		pct=$(awk -v d="$diff_px" -v w="$rw" -v h="$rh" 'BEGIN { printf "%.3f", 100 * d / (w * h) }')
		awk -v p="$pct" -v m="$max_pct" 'BEGIN { exit !(p > m) }' && verdict="ABOVE LIMIT (report-only)"
		echo "reference diff: ${pct}% of ${rw}x${rh} px differ (fuzz $fuzz, limit ${max_pct}%) vs $ref: $verdict"
	else
		echo "reference diff: comparison failed for $ref: $diff_px"
	fi
}

# Report-only: whatever state the reference or ImageMagick is in, this block must not change the exit code.
if [[ -n "$ref" ]]; then
	compare_reference "$ref" || echo "reference diff: compare failed (ignored)"
fi

exit "$fail"
