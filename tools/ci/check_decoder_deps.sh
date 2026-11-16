#!/usr/bin/env bash
# Checks that the model decoder (#647) needs no engine, GL or window library directly (#1077; NEEDED only, not what
# those libraries pull in):
#
#   tools/ci/check_decoder_deps.sh <path to snk_model_decoder>
#
# readelf -d: no NEEDED library is the shared engine (libsnk_engine*), the model import library that links it
# (libsnk_model_import_assimp*), GL (libGL*, libOpenGL*, libEGL*, libGLX*), X11/Wayland/xcb or GLFW. The decoder links its
# import core, snk_model_codec, common, the logger, platform and assimp statically. Linux (ELF) only; the Windows twin is
# tools/ci/check_windows_deps.sh --deny.
set -euo pipefail

bin="${1:?usage: check_decoder_deps.sh <snk_model_decoder>}"
if [ ! -f "$bin" ]; then
	echo "::error::no decoder binary at '$bin'"
	exit 2
fi

dynamic="$(readelf -d "$bin")"
needed="$(grep -F '(NEEDED)' <<<"$dynamic" || true)"
# A static binary has no NEEDED entry at all, and readelf printing nothing would pass the check on nothing.
if ! grep -q 'Dynamic section' <<<"$dynamic"; then
	echo "::error::readelf found no dynamic section in '$bin': the check cannot run"
	exit 2
fi
echo "NEEDED libraries of $bin:"
echo "${needed:-  (none)}"
bad="$(grep -E '\[lib(snk_engined?|snk_model_import_assimpd?|GL|OpenGL|EGL|GLX|GLdispatch|X11|X11-xcb|Xext|Xrandr|Xi|Xcursor|Xinerama|xcb[-a-z0-9]*|wayland-[a-z]+|glfw)\.so' <<<"$needed" || true)"
if [ -n "$bad" ]; then
	echo "::error::the model decoder needs a library it must not load:"
	echo "$bad"
	exit 1
fi
echo "OK: no snk_engine, model import, GL, X11/Wayland or GLFW library"
