#!/usr/bin/env bash
# Usage: crop_montage.sh OUT.png WxH+X+Y IN1.png [IN2.png ...]
# Crops the same region from each screenshot, upscales 2x and joins them side by side.
set -euo pipefail
out="$1"; geom="$2"; shift 2
parts=()
i=0
for f in "$@"; do
	p="${out%.png}_part$i.png"
	convert "$f" -crop "$geom" +repage -scale 200% "$p"
	parts+=("$p")
	i=$((i + 1))
done
convert "${parts[@]}" +append "$out"
rm -f "${parts[@]}"
