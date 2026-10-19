#!/usr/bin/env bash
# Sanity checks for a headless editor run (see tools/run_headless.sh).
#
# Usage: check_smoke.sh SCREENSHOT LOG [GEOMETRY [MIN_STDDEV [REFERENCE]]]
#   GEOMETRY    ImageMagick crop of the viewport centre   (default: 200x200+860+450)
#   MIN_STDDEV  minimum normalized pixel standard deviation of that crop (default: 0.055)
#   REFERENCE   reference PNG (tools/headless/reference: a downscaled llvmpipe screenshot); the screenshot is
#               scaled to its size and compared. The references are only comparable for runs made with
#               run_headless.sh --frames (constant frame dt); with wall-clock time the robot pose varies. GATING: above the limit, or a missing/unreadable reference, fails
#               the script (::error:: line); the diff image is saved as <screenshot>.diff.png. SNK_SMOKE_FUZZ (per-pixel colour tolerance, default 4%) and SNK_SMOKE_MAX_DIFF_PCT
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
# Gating: a comparison above the limit, or a missing/unreadable shot or reference, sets fail=1 and prints a
# GitHub Actions ::error:: line. The diff image is written next to the shot (<shot>.diff.png) for the artifact upload.
compare_reference() {
	local ref="$1" fuzz="${SNK_SMOKE_FUZZ:-4%}" max_pct="${SNK_SMOKE_MAX_DIFF_PCT:-2}"
	if [[ ! -s "$ref" ]]; then
		echo "::error::reference diff: reference $ref is missing or empty"
		fail=1
		return 0
	fi
	local size rw rh diff_px pct verdict=ok diff_img="${shot%.png}.diff.png"
	if ! size=$(identify -format '%w %h' "$ref"); then
		echo "::error::reference diff: cannot read reference $ref"
		fail=1
		return 0
	fi
	read -r rw rh <<<"$size"
	diff_px=$(convert "$shot" -resize "${rw}x${rh}!" png:- | compare -metric AE -fuzz "$fuzz" "$ref" png:- "$diff_img" 2>&1 || true)
	diff_px="${diff_px%% *}"
	if [[ "$diff_px" =~ ^[0-9]+$ && "${rw:-}" =~ ^[0-9]+$ && "${rh:-}" =~ ^[0-9]+$ ]]; then
		pct=$(awk -v d="$diff_px" -v w="$rw" -v h="$rh" 'BEGIN { printf "%.3f", 100 * d / (w * h) }')
		awk -v p="$pct" -v m="$max_pct" 'BEGIN { exit !(p > m) }' && verdict="ABOVE LIMIT"
		echo "reference diff: ${pct}% of ${rw}x${rh} px differ (fuzz $fuzz, limit ${max_pct}%) vs $ref: $verdict"
		if [[ "$verdict" != ok ]]; then
			echo "::error::reference diff ${pct}% > ${max_pct}% for $shot vs $ref (diff image: $diff_img)"
			fail=1
		fi
	else
		echo "::error::reference diff: comparison failed for $ref: $diff_px"
		fail=1
	fi
}

if [[ -n "$ref" ]]; then
	compare_reference "$ref"
fi

exit "$fail"
