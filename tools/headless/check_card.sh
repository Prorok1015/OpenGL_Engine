#!/usr/bin/env bash
# Pixel checks of the card sample (#1197): the in-process capture (capture/on_settled) of samples/card/card_demo.desc.
#
# Usage: check_card.sh SCREENSHOT
#
# The level's layout (samples repo, card/card_demo.desc; documented in card/tools/gen_card_textures.py): the editor's
# Scene view camera at the origin looking down -Z, a card showing its face on the left, a card showing its back on the
# right, and in the middle a lone frame plane ("Frame probe") in front of a green plane ("Backdrop"). The regions are
# fractions of the image (x0,y0,x1,y1 in 0..1), so they do not depend on the size of the capture, only on its aspect
# (about 1.79:1 at run_headless.sh --size 1600x900). Each one can be overridden by an environment variable:
#   SNK_CARD_BG        clear colour, nothing drawn                 (default 0.02,0.03,0.10,0.12)
#   SNK_CARD_FACE      middle of the face card                     (default 0.20,0.42,0.24,0.58)
#   SNK_CARD_BACK      middle of the back card                     (default 0.76,0.42,0.80,0.58)
#   SNK_CARD_HOLE      the frame probe's transparent centre        (default 0.47,0.40,0.53,0.60)
#   SNK_CARD_BEHIND    the backdrop beside the probe, no frame     (default 0.32,0.40,0.37,0.60)
#   SNK_CARD_BAND      the probe's left opaque band (gold)         (default 0.395,0.40,0.408,0.60)
# Checks, on the mean colour (0..255 per channel) of each region:
#   (a) FACE differs from BG by more than SNK_CARD_MIN_DIFF (default 60, euclidean RGB) and its red exceeds its blue
#       by SNK_CARD_MIN_RED (default 40): the face card is drawn with the face texture, not the back or a fallback;
#   (b) BEHIND is green-dominant (green exceeds red and blue by SNK_CARD_MIN_GREEN, default 40), HOLE matches BEHIND
#       within SNK_CARD_MAX_HOLE_DIFF (default 30) and BAND differs from BEHIND by more than SNK_CARD_MIN_DIFF: the
#       frame is drawn, and its transparent centre shows the backdrop behind it;
# The image's aspect must be within SNK_CARD_ASPECT (default 1.70,1.90): the regions assume that layout.
#   (c) BACK has the back texture's dominant blue: blue exceeds red and green by SNK_CARD_MIN_BLUE (default 40).
# A failed check prints an ::error:: line; the crops are written next to the shot (<shot>.<region>.png) for the
# artifact. Needs ImageMagick (convert, identify).
set -euo pipefail

shot="${1:?screenshot}"
[[ -s "$shot" ]] || { echo "::error::card check: no screenshot: $shot"; exit 1; }

min_diff="${SNK_CARD_MIN_DIFF:-60}"
max_hole_diff="${SNK_CARD_MAX_HOLE_DIFF:-30}"
min_blue="${SNK_CARD_MIN_BLUE:-40}"
min_red="${SNK_CARD_MIN_RED:-40}"
min_green="${SNK_CARD_MIN_GREEN:-40}"
fail=0

read -r width height < <(identify -format '%w %h\n' "$shot" | head -1)
echo "card check: $shot is ${width}x${height}"
aspect=$(awk -v w="$width" -v h="$height" 'BEGIN { printf "%.3f", w / h }')
IFS=, read -r aspect_min aspect_max <<<"${SNK_CARD_ASPECT:-1.70,1.90}"
echo "aspect $aspect (expected $aspect_min..$aspect_max)"
if ! awk -v a="$aspect" -v lo="$aspect_min" -v hi="$aspect_max" 'BEGIN { exit !(a >= lo && a <= hi) }'; then
	echo "::error::card check: aspect $aspect is outside $aspect_min..$aspect_max, the regions do not fit this shot"
	exit 1
fi

# "x0,y0,x1,y1" fractions -> an ImageMagick crop geometry in pixels.
geometry() {
	awk -v r="$1" -v w="$width" -v h="$height" 'BEGIN {
		split(r, f, ","); x0 = int(f[1] * w); y0 = int(f[2] * h); x1 = int(f[3] * w); y1 = int(f[4] * h)
		if (x1 <= x0) x1 = x0 + 1; if (y1 <= y0) y1 = y0 + 1
		printf "%dx%d+%d+%d", x1 - x0, y1 - y0, x0, y0 }'
}

# The mean colour of a region as "r g b"; the crop is kept for the artifact.
mean_rgb() {
	local name="$1" region="$2" geom
	geom=$(geometry "$region")
	convert "$shot" -alpha off -crop "$geom" +repage "${shot%.png}.${name}.png"
	convert "${shot%.png}.${name}.png" -scale '1x1!' -format '%[fx:int(255*r+0.5)] %[fx:int(255*g+0.5)] %[fx:int(255*b+0.5)]' info:
}

distance() {
	awk -v a="$1" -v b="$2" 'BEGIN { split(a, p, " "); split(b, q, " ");
		printf "%.1f", sqrt((p[1]-q[1])^2 + (p[2]-q[2])^2 + (p[3]-q[3])^2) }'
}

above() { awk -v v="$1" -v m="$2" 'BEGIN { exit !(v > m) }'; }

bg=$(mean_rgb bg "${SNK_CARD_BG:-0.02,0.03,0.10,0.12}")
face=$(mean_rgb face "${SNK_CARD_FACE:-0.20,0.42,0.24,0.58}")
back=$(mean_rgb back "${SNK_CARD_BACK:-0.76,0.42,0.80,0.58}")
hole=$(mean_rgb hole "${SNK_CARD_HOLE:-0.47,0.40,0.53,0.60}")
behind=$(mean_rgb behind "${SNK_CARD_BEHIND:-0.32,0.40,0.37,0.60}")
band=$(mean_rgb band "${SNK_CARD_BAND:-0.395,0.40,0.408,0.60}")
echo "mean rgb: bg=($bg) face=($face) back=($back) hole=($hole) behind=($behind) band=($band)"

# (a) the face card is visible.
d=$(distance "$face" "$bg")
echo "(a) face vs background: $d (min $min_diff)"
if ! above "$d" "$min_diff"; then
	echo "::error::card check (a): the face card region looks like the background (distance $d <= $min_diff)"
	fail=1
fi
read -r r g b <<<"$face"
echo "(a) face card: red $r vs blue $b (min margin $min_red)"
if ! above "$((r - b))" "$min_red"; then
	echo "::error::card check (a): the face card region is not the face texture's red (rgb $face)"
	fail=1
fi

# (b) the frame is drawn, and its transparent centre shows the backdrop behind it.
read -r r g b <<<"$behind"
echo "(b) backdrop: green $g vs red $r, blue $b (min margin $min_green)"
if ! { above "$((g - r))" "$min_green" && above "$((g - b))" "$min_green"; }; then
	echo "::error::card check (b): the backdrop region is not green (rgb $behind)"
	fail=1
fi
d=$(distance "$band" "$behind")
echo "(b) frame band vs backdrop: $d (min $min_diff)"
if ! above "$d" "$min_diff"; then
	echo "::error::card check (b): the frame probe's opaque band is not drawn over the backdrop (distance $d <= $min_diff)"
	fail=1
fi
d=$(distance "$hole" "$behind")
echo "(b) frame centre vs backdrop: $d (max $max_hole_diff)"
if above "$d" "$max_hole_diff"; then
	echo "::error::card check (b): the frame's transparent centre does not show the backdrop behind it (distance $d > $max_hole_diff)"
	fail=1
fi

# (c) the back side is visible: the back texture's dominant blue.
read -r r g b <<<"$back"
echo "(c) back card: blue $b vs red $r, green $g (min margin $min_blue)"
if ! { above "$((b - r))" "$min_blue" && above "$((b - g))" "$min_blue"; }; then
	echo "::error::card check (c): the back card region is not the back texture's blue (rgb $back)"
	fail=1
fi

exit "$fail"
