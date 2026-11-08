#!/usr/bin/env bash
# Run the editor without a GPU or monitor: Xvfb (virtual X server) + Mesa llvmpipe (software OpenGL 4.5).
# Optionally replays an action script (xdotool) and takes screenshots of the virtual screen.
#
# Usage:
#   tools/run_headless.sh [options] [-- extra editor args]
# Options:
#   --editor PATH      editor executable            (default: build/linux/Editor/editor)
#   --config PATH      editor config                (default: Editor/config/editor.cfg)
#   --actions FILE     action script, one command per line (see below)
#   --seconds N        run time when no action script is given (default: 20)
#   --shot PATH        final screenshot             (default: headless_out/final.png); editor.log and
#                      xvfb.log go next to it
#   --size WxH         virtual screen size          (default: 1600x900)
#   --frames N         deterministic time: the editor advances a constant --frame-dt per frame and freezes
#                      scene time N frames after the level is instantiated; the script waits until that and all
#                      loads are done before the actions/screenshot,
#                      so a shot no longer depends on runner speed. With no action script --seconds is ignored.
#   --frame-dt SEC     frame step used by --frames  (default: 0.0166667)
#   --wait-for-log RE  with --frames: after the scene settled, also wait until the editor log has a line matching RE
#                      (e.g. "captured viewport" of capture/on_settled), so the run is not stopped before it happens
#
# Action script commands (lines starting with # are comments):
#   sleep SECONDS            wait (fractions allowed)
#   click X Y [BUTTON]       move to X,Y and click (BUTTON 1=left default, 3=right)
#   dclick X Y               double click
#   drag X1 Y1 X2 Y2         press at X1,Y1, move to X2,Y2, release
#   move X Y                 move the mouse
#   key KEYS                 xdotool key syntax, e.g. ctrl+s, Delete, Escape
#   type TEXT                type text
#   shot PATH                screenshot of the whole virtual screen
#
# Requirements (Debian/Ubuntu): xvfb libgl1-mesa-dri xdotool imagemagick
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
EDITOR="$ROOT/build/linux/Editor/editor"
CONFIG="$ROOT/Editor/config/editor.cfg"
ACTIONS=""
SECONDS_TO_RUN=20
SHOT="headless_out/final.png"
SIZE="1600x900"
FRAMES=""
FRAME_DT="0.0166667"
WAIT_LOG=""
EXTRA=()

while [[ $# -gt 0 ]]; do
	case "$1" in
		--editor) EDITOR="$2"; shift 2 ;;
		--config) CONFIG="$2"; shift 2 ;;
		--actions) ACTIONS="$2"; shift 2 ;;
		--seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
		--shot) SHOT="$2"; shift 2 ;;
		--size) SIZE="$2"; shift 2 ;;
		--frames) FRAMES="$2"; shift 2 ;;
		--frame-dt) FRAME_DT="$2"; shift 2 ;;
		--wait-for-log) WAIT_LOG="$2"; shift 2 ;;
		--) shift; EXTRA=("$@"); break ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

# 0 would mean "never freeze", and the run would wait out the whole settle timeout.
if [[ -n "$FRAMES" && ! "$FRAMES" =~ ^[1-9][0-9]*$ ]]; then
	echo "--frames needs a positive integer, got: $FRAMES" >&2
	exit 2
fi

for tool in Xvfb xdotool import; do
	command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 2; }
done

mkdir -p "$(dirname "$SHOT")"
LOG="$(dirname "$SHOT")/editor.log"
XVFB_LOG="$(dirname "$SHOT")/xvfb.log"
DISPLAY_FILE="$(mktemp)"

# -noreset: by default the server resets when its last client disconnects; the readiness probe below is that client,
# and the editor connecting during the reset failed with "X11: Failed to open display" (#630). -displayfd: Xvfb picks
# a free display itself and writes its number once it accepts connections (no race on /tmp/.X<N>-lock).
Xvfb -displayfd 3 -noreset -screen 0 "${SIZE}x24" -nolisten tcp 3>"$DISPLAY_FILE" >"$XVFB_LOG" 2>&1 &
XVFB_PID=$!
EDITOR_PID=""
cleanup() {
	[[ -n "$EDITOR_PID" ]] && kill "$EDITOR_PID" 2>/dev/null || true
	kill "$XVFB_PID" 2>/dev/null || true
	wait 2>/dev/null || true
	rm -f "$DISPLAY_FILE"
}
trap cleanup EXIT

xvfb_report() {
	echo "Xvfb (pid $XVFB_PID, display ${DISPLAY:-?}): $(kill -0 "$XVFB_PID" 2>/dev/null && echo alive || echo exited); log: $XVFB_LOG" >&2
	tail -20 "$XVFB_LOG" >&2 || true
}

for _ in $(seq 1 200); do
	kill -0 "$XVFB_PID" 2>/dev/null || { echo "error: Xvfb exited before it was ready" >&2; xvfb_report; exit 1; }
	[[ -s "$DISPLAY_FILE" ]] && break
	sleep 0.1
done
DISPLAY_NUM="$(head -n 1 "$DISPLAY_FILE" 2>/dev/null || true)"
[[ "$DISPLAY_NUM" =~ ^[0-9]+$ ]] || { echo "error: Xvfb gave no display number after 20 s" >&2; xvfb_report; exit 1; }
export DISPLAY=":${DISPLAY_NUM}"

if [[ -n "$FRAMES" ]]; then
	EXTRA+=(--set app/fixed_frame_dt "$FRAME_DT" app/freeze_after_frames "$FRAMES")
fi

# Every run checks its display right before the editor starts: the editor aborts when it cannot open it.
if ! kill -0 "$XVFB_PID" 2>/dev/null || ! xdotool getdisplaygeometry >/dev/null 2>&1; then
	echo "error: Xvfb on $DISPLAY does not answer before the editor starts" >&2
	xvfb_report
	exit 1
fi

# Software rasterizer; vsync off so the virtual display does not throttle frames.
LIBGL_ALWAYS_SOFTWARE=1 vblank_mode=0 \
	"$EDITOR" --config "$CONFIG" --non-interactive "${EXTRA[@]}" >"$LOG" 2>&1 &
EDITOR_PID=$!

alive() { kill -0 "$EDITOR_PID" 2>/dev/null; }

# Wait for the editor window so actions do not race window creation.
for _ in $(seq 1 60); do
	alive || break
	xdotool search --onlyvisible --name . >/dev/null 2>&1 && break
	sleep 0.5
done
if ! alive; then
	rc=0
	wait "$EDITOR_PID" 2>/dev/null || rc=$?
	EDITOR_PID=""
	echo "error: the editor exited before its window appeared (code $rc); log: $LOG" >&2
	tail -20 "$LOG" >&2
	xvfb_report
	exit 1
fi

if [[ -n "$FRAMES" ]]; then
	# Logged by app::loop_service once time is frozen and no loads are in flight; a slow runner just takes longer.
	for _ in $(seq 1 480); do
		alive || break
		grep -q 'deterministic scene settled' "$LOG" && break
		sleep 0.5
	done
	if ! grep -q 'deterministic scene settled' "$LOG"; then
		echo "error: scene did not settle within 240 s (editor alive: $(alive && echo yes || echo no)); log: $LOG" >&2
		tail -20 "$LOG" >&2
		exit 1
	fi
	sleep 1 # a few frozen frames, so ImGui hover/fade state settles
	if [[ -n "$WAIT_LOG" ]]; then
		for _ in $(seq 1 480); do
			alive || break
			grep -q -- "$WAIT_LOG" "$LOG" && break
			sleep 0.5
		done
		if ! grep -q -- "$WAIT_LOG" "$LOG"; then
			echo "error: no line matching '$WAIT_LOG' within 240 s (editor alive: $(alive && echo yes || echo no)); log: $LOG" >&2
			tail -20 "$LOG" >&2
			exit 1
		fi
	fi
fi

if [[ -n "$ACTIONS" ]]; then
	while IFS= read -r line || [[ -n "$line" ]]; do
		line="${line%%#*}"; read -r cmd a b c d <<<"$line" || true
		[[ -z "${cmd:-}" ]] && continue
		alive || { echo "editor exited during actions" >&2; break; }
		case "$cmd" in
			sleep) sleep "$a" ;;
			click) xdotool mousemove "$a" "$b"; sleep 0.4; xdotool click "${c:-1}" ;; # hover first: ImGui needs a frame at the new position
			dclick) xdotool mousemove "$a" "$b"; sleep 0.4; xdotool click --repeat 2 --delay 80 1 ;;
			drag) xdotool mousemove "$a" "$b" sleep 0.4 mousedown 1 sleep 0.2 mousemove "$c" "$d" sleep 0.2 mouseup 1 ;;
			move) xdotool mousemove "$a" "$b" ;;
			key) xdotool key --delay 50 "$a" ;;
			type) xdotool type --delay 30 "${line#*type }" ;;
			shot) mkdir -p "$(dirname "$a")"; import -window root "$a" ;;
			*) echo "unknown action: $cmd" >&2 ;;
		esac
	done <"$ACTIONS"
elif [[ -z "$FRAMES" ]]; then
	sleep "$SECONDS_TO_RUN"
fi

if alive; then
	import -window root "$SHOT"
	echo "screenshot: $SHOT"
	echo "log: $LOG"
else
	wait "$EDITOR_PID" 2>/dev/null; rc=$?
	EDITOR_PID=""
	echo "editor exited early (code $rc); log: $LOG" >&2
	tail -20 "$LOG" >&2
	exit 1
fi
