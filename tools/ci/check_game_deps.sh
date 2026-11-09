#!/usr/bin/env bash
# Checks that the shipping host `game` (#591) carries nothing of the editor:
#
#   tools/ci/check_game_deps.sh <path to the game binary>
#
# 1. readelf -d: no NEEDED library is the shared engine (snk_engine), an editor library or a git library.
# 2. nm -C --defined-only: no symbol is defined in the editor's namespaces (edt::, editor::, components::: the editor
#    system, its panels and ImGui code) or in the MCP server's (mcp::), counting vtables, typeinfo and guard variables.
# Linux (ELF) only.
set -euo pipefail

bin="${1:?usage: check_game_deps.sh <game binary>}"
if [ ! -f "$bin" ]; then
	echo "::error::no game binary at '$bin'"
	exit 2
fi

status=0

needed="$(readelf -d "$bin" | grep -F '(NEEDED)' || true)"
echo "NEEDED libraries of $bin:"
echo "${needed:-  (none)}"
bad_needed="$(echo "$needed" | grep -Ei 'snk_engine|editor|git' || true)"
if [ -n "$bad_needed" ]; then
	echo "::error::the game host needs a library it must not load:"
	echo "$bad_needed"
	status=1
fi

symbols="$(nm -C --defined-only "$bin" || true)"
# A stripped binary or a failing nm would make the checks below pass on nothing.
if ! grep -qE ' [A-Za-z] scn::' <<<"$symbols"; then
	echo "::error::nm found no engine symbol (scn::) in '$bin': the symbol check cannot run"
	exit 2
fi
pattern=' [A-Za-z] ((vtable|typeinfo name|typeinfo|VTT|construction vtable|guard variable) for )?(edt|editor|components|mcp)::'
bad_symbols="$(echo "$symbols" | grep -E "$pattern" || true)"
if [ -n "$bad_symbols" ]; then
	count="$(echo "$bad_symbols" | wc -l)"
	echo "::error::the game host defines ${count} editor or MCP symbols (first 50):"
	echo "$bad_symbols" | head -50 || true
	status=1
fi

if [ "$status" -eq 0 ]; then
	echo "OK: no snk_engine/editor/git library, no edt::/editor::/components::/mcp:: symbol"
fi
exit "$status"
