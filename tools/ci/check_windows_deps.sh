#!/usr/bin/env bash
# Fails when a binary of the Windows package needs a DLL that is neither in the package nor part of Windows (#223).
#
#   tools/ci/check_windows_deps.sh [--runtime-elsewhere] <bin dir> <file>...
#   tools/ci/check_windows_deps.sh --deny <dll>[,<dll>...] <bin dir> <file>...
#
# --runtime-elsewhere: the binary is checked where it was built, not in a package (the shipping host `game`, #591,
# has no package yet), so the MSVC runtime is reported and not required next to it. Everything else is checked alike.
# --deny: only this: fails when a file imports (or delay-loads) one of the listed DLLs, compared without case. The model
# decoder must import no window, GDI, GL or engine DLL directly (#1077). Only direct imports are seen: what an imported
# system DLL loads in turn (shell32 -> user32) is not, so this is no proof for win32k lockdown (#1054).
#
# Reads the imports (and delay-load imports) with `dumpbin -dependents` (an MSVC developer shell). A dependency is
# fine when it sits in <bin dir> next to the binary, or is on the explicit list of Windows DLLs below (API sets
# included). Anything else fails, the MSVC runtime too (vcruntime*, msvcp*, ...): the runner has it in System32,
# an end user's machine may not. Extend the list only with a DLL that ships with every Windows 10/11.
set -euo pipefail

runtime_elsewhere=0
deny=""
if [[ "${1:-}" == --runtime-elsewhere ]]; then
	runtime_elsewhere=1
	shift
elif [[ "${1:-}" == --deny ]]; then
	deny="${2:?usage: check_windows_deps.sh --deny <dll>[,<dll>...] <bin dir> <file>...}"
	shift 2
fi
bin="${1:?usage: check_windows_deps.sh [--runtime-elsewhere] <bin dir> <file>...}"
shift
[[ $# -gt 0 ]] || { echo "usage: check_windows_deps.sh [--runtime-elsewhere] <bin dir> <file>..."; exit 2; }
# Exactly what editor.exe and snk_engine.dll import today (dumpbin, #223): a new import fails until it is added here.
# opengl32.dll is not imported: GLFW and glad load it at run time.
# userenv.dll: the AppContainer profile calls of the decoder sandbox in plt_process (#1134, #1178).
windows_dlls=" kernel32.dll user32.dll gdi32.dll shell32.dll advapi32.dll imm32.dll ws2_32.dll mswsock.dll ntdll.dll userenv.dll "
# One space-separated line, so " name " matches a whole entry.
windows_dlls=" $(echo ${windows_dlls}) "
# The same for the denied DLLs, in lower case.
denied=" $(echo "${deny,,}" | tr ',' ' ') "

status=0
for f in "$@"; do
	if [[ ! -f "${bin}/${f}" ]]; then
		echo "::error::${bin}/${f} does not exist"
		status=1
		continue
	fi
	echo "${f}:"
	deps="$(dumpbin -nologo -dependents "${bin}/${f}" | tr -d '\r' | awk '
		/Image has the following (delay load )?dependencies/ { take = 1; next }
		/Summary/ { take = 0 }
		take && NF == 1 { print $1 }')"
	if [[ -z "$deps" ]]; then
		echo "::error::dumpbin listed no dependencies of ${f}"
		status=1
		continue
	fi
	while IFS= read -r dep; do
		lower="${dep,,}"
		if [[ -n "$deny" ]]; then
			if [[ "$denied" == *" ${lower} "* ]]; then
				echo "::error::${f} imports ${dep}, which it must not load (tools/ci/check_windows_deps.sh --deny)"
				status=1
			else
				echo "  ${dep}"
			fi
			continue
		fi
		if [[ -f "${bin}/${dep}" ]]; then
			echo "  ${dep}: in the package"
		elif [[ "$lower" =~ ^(vcruntime|msvcp|concrt|vccorlib|vcomp) || ( $runtime_elsewhere -eq 1 && "$lower" =~ ^ucrtbase ) ]]; then
			# ucrtbased.dll: a /MDd build imports the debug UCRT by name; it is never part of a package.
			if [[ $runtime_elsewhere -eq 1 ]]; then
				echo "  ${dep}: MSVC runtime (not checked here: --runtime-elsewhere)"
			else
				echo "::error::${f} needs ${dep} (MSVC runtime) but it is not in ${bin}"
				status=1
			fi
		elif [[ "$lower" == api-ms-win-* || "$lower" == ext-ms-* ]]; then
			echo "  ${dep}: Windows API set"
		elif [[ "$windows_dlls" == *" ${lower} "* ]]; then
			echo "  ${dep}: Windows"
		else
			echo "::error::${f} needs ${dep}: it is neither in ${bin} nor on the list of Windows DLLs (tools/ci/check_windows_deps.sh)"
			status=1
		fi
	done <<< "$deps"
done
exit $status
