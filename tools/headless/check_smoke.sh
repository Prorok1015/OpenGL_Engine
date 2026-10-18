#!/usr/bin/env bash
# Sanity checks for a headless editor run (see tools/run_headless.sh).
#
# Usage: check_smoke.sh SCREENSHOT LOG [GEOMETRY [MIN_STDDEV]]
#   GEOMETRY    ImageMagick crop of the viewport centre   (default: 200x200+860+450)
#   MIN_STDDEV  minimum normalized pixel standard deviation of that crop (default: 0.055)
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

exit "$fail"
