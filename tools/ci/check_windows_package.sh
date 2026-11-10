#!/usr/bin/env bash
# Checks the Windows release package the way a user gets it (#223):
#
#   tools/ci/check_windows_package.sh <Snake-*-windows-x64.zip> <Snake-*-windows-x64-debug.zip> <debug-targets dir>
#
# 0. The Debug package was built with the same configuration-independent targets files as the Release one (the
#    debug archive only adds *-debug.cmake files, which pair with the Release package's snake_sdk_*Targets.cmake).
# 1. The debug archive unpacks over the main one without replacing any of its files.
# 2. The layout: the editor, snk_engine.dll and the Debug snk_engined.dll, PDBs, the MSVC runtime, the SDK.
# 3. editor.exe and snk_engine.dll need no DLL outside the package but Windows' own (check_windows_deps.sh).
# 4. tests/package_consumer builds and runs against snk::engine and snk::engine_static, Debug and Release.
# 5. The project template's game module builds against the package (Release, and DebugGame on the Release engine), and
#    its shipping executable (preset `shipping`, #592): game.exe with the static engine, built only (tests/static_game
#    runs a host), needing no snk_engine.dll nor any other DLL but Windows' own and the MSVC runtime.
# 6. editor.exe runs from the unpacked folder: --headless (a null window and the null renderer, no GL context, #452)
#    opens projects/sandbox, loads a level and exits with 0. That proves the DLLs resolve and the config, the engine
#    resources (res_core/) and the project are found; it does not draw anything (hosted runners have no GPU).
# Run from the repository root in an MSVC developer shell (cl, ninja, dumpbin, 7z on PATH).
set -euo pipefail

usage="usage: check_windows_package.sh <main zip> <debug zip> <debug-targets dir>"
main_zip="${1:?$usage}"
debug_zip="${2:?$usage}"
debug_targets="${3:?$usage}"
work="$(cygpath -m "${RUNNER_TEMP:-$PWD/.tmp}")/package_check"
rm -rf "$work"
mkdir -p "$work"

# ---- 1. unpack, the debug archive on top ----
7z x -y -o"${work}/main" "$main_zip" > /dev/null
7z x -y -o"${work}/debug" "$debug_zip" > /dev/null
roots=("${work}"/main/Snake/*)
[[ ${#roots[@]} -eq 1 && -d "${roots[0]}" ]] || { echo "::error::the main archive must hold exactly one Snake/<version>/"; exit 1; }
root="${roots[0]}"
ver="$(basename "$root")"
[[ -d "${work}/debug/Snake/${ver}" ]] || { echo "::error::the debug archive has no Snake/${ver}/"; exit 1; }
overlap="$(comm -12 <(cd "$root" && find . -type f | sort) <(cd "${work}/debug/Snake/${ver}" && find . -type f | sort))"
if [[ -n "$overlap" ]]; then
	echo "::error::the debug archive would replace files of the main one:"
	echo "$overlap"
	exit 1
fi
main_files="$(cd "$root" && find . -type f | sort)"
before="$(cd "$root" && tr '\n' '\0' <<< "$main_files" | xargs -0 sha256sum)"
7z x -y -o"${work}/main" "$debug_zip" > /dev/null
after="$(cd "$root" && tr '\n' '\0' <<< "$main_files" | xargs -0 sha256sum)"
if [[ "$before" != "$after" ]]; then
	echo "::error::unpacking the debug archive changed files of the main one"
	diff <(echo "$before") <(echo "$after") || true
	exit 1
fi
echo "Snake/${ver}: $(wc -l <<< "$main_files") files, the debug archive adds $(cd "${work}/debug/Snake/${ver}" && find . -type f | wc -l)"

# Sections 0-3 report every failure before the script stops, so one run also lists the DLL dependencies.
status=0

# ---- 0. the Debug and Release packages agree on everything but the per-configuration files ----
for f in snake_sdk_sharedTargets.cmake snake_sdk_staticTargets.cmake; do
	if ! cmp -s "${debug_targets}/${f}" "${root}/lib/cmake/snake_sdk/${f}"; then
		echo "::error::${f} of the Debug build differs from the Release package's: a definition or target depends on the configuration"
		diff "${debug_targets}/${f}" "${root}/lib/cmake/snake_sdk/${f}" | head -40 || true
		status=1
	fi
done

# ---- 2. layout ----
for f in bin/editor.exe bin/editor.pdb bin/snk_model_decoder.exe bin/snk_model_decoder.pdb bin/snk_engine.dll bin/snk_engine.pdb bin/snk_engined.dll bin/snk_engined.pdb \
	bin/snk_editor_api.dll bin/snk_editor_api.pdb bin/snk_editor_apid.dll bin/snk_editor_apid.pdb \
	lib/snk/snk_editor_api.lib lib/snk/snk_editor_apid.lib include/snk/editor/edt_api.h \
	bin/vcruntime140.dll bin/msvcp140.dll lib/snk/snk_engine.lib lib/snk/snk_engined.lib \
	lib/cmake/snake_sdk/snake_sdkConfig.cmake lib/cmake/snake_sdk/snake_sdk_sharedTargets.cmake \
	lib/cmake/snake_sdk/snake_sdk_staticTargets.cmake lib/cmake/snake_sdk/snake_sdk_sharedTargets-release.cmake \
	lib/cmake/snake_sdk/snake_sdk_sharedTargets-debug.cmake lib/cmake/snake_sdk/snake_sdk_staticTargets-release.cmake \
	lib/cmake/snake_sdk/snake_sdk_staticTargets-debug.cmake lib/cmake/snake_sdk/SnkGameHost.cmake \
	include/snk/game_host/game_host.h config/editor.cfg data/templates res_core \
	projects/sandbox/project.snkproj README.txt include/snk; do
	if [[ ! -e "${root}/${f}" ]]; then echo "::error::the package has no ${f}"; status=1; fi
done
if ls "${root}/bin" | grep -Ei '^(vcruntime|msvcp|concrt|vccorlib)[0-9_]*d\.dll$'; then
	echo "::error::the debug MSVC runtime is not redistributable"; status=1
fi
# The decoder's test build has flags that sleep, allocate and start copies of itself: it is for the unit tests only.
if find "${root}" -iname 'snk_model_decoder_selftest*' | grep -q .; then
	echo "::error::the package holds the decoder's self-test build (snk_model_decoder_selftest)"; status=1
fi
ls -l "${root}/bin"

# ---- 3. dependencies ----
bash tools/ci/check_windows_deps.sh "${root}/bin" editor.exe snk_model_decoder.exe snk_engine.dll snk_editor_api.dll || status=1
[[ $status -eq 0 ]] || exit 1

# ---- 4. consumer, both engines, both configurations ----
header_check=ON
for config in Release Debug; do
	for engine in shared static; do
		dir="${work}/consumer-${engine}-${config}"
		echo "---- package_consumer: ${engine} engine, ${config}"
		cmake -S tests/package_consumer -B "$dir" -G Ninja "-DCMAKE_BUILD_TYPE=${config}" "-DCMAKE_PREFIX_PATH=${root}" \
			"-DSNK_CONSUMER_ENGINE=${engine}" "-DSNK_CONSUMER_HEADER_CHECK=${header_check}"
		cmake --build "$dir"
		"${dir}/package_consumer.exe"
		header_check=OFF
	done
done

# ---- 5. the project template's game module ----
proj="${work}/template_project"
cp -r Editor/data/templates/projects/empty "$proj"
export SNAKE_SDK_DIR="${root}/lib/cmake/snake_sdk"
for preset in release debuggame; do
	(cd "${proj}/code" && cmake --preset "$preset" && cmake --build --preset "$preset")
	if ! grep -qi 'snk_engine\.lib' "${proj}/build/${preset}/build.ninja"; then
		echo "::error::${preset}: the game module does not link the Release engine (snk_engine.lib)"; exit 1
	fi
done
ls -l "${proj}/build/bin"
test -f "${proj}/build/bin/game.dll"
test -f "${proj}/build/bin/game.pdb"
# No pipe into grep -q: under pipefail an early exit of the reader fails the writer, and the check would pass.
if [[ -n "$(find "${proj}/build/bin" -maxdepth 1 -iname 'gamed.*' -print -quit)" ]]; then
	echo "::error::the module has a debug postfix"; exit 1
fi
# DebugGame (built last) names its PDB without a path (/PDBALTPATH, #435).
if grep -aqi 'bin[\/]game\.pdb' "${proj}/build/bin/game.dll"; then echo "::error::game.dll names its PDB by path"; exit 1; fi
echo "---- the template's shipping executable"
(cd "${proj}/code" && cmake --preset shipping && cmake --build --preset shipping)
ls -l "${proj}/build/shipping"
test -f "${proj}/build/shipping/game.exe"
# No snk_engine.dll lies next to it: an import of the shared engine fails here like any other non-Windows DLL.
bash tools/ci/check_windows_deps.sh --runtime-elsewhere "${proj}/build/shipping" game.exe

# ---- 6. editor.exe from the unpacked folder ----
log="${work}/editor_headless"
rc=0
(cd "$root" && timeout 180 bin/editor.exe --config config/editor.cfg --headless --project projects/sandbox \
	--set editor/startup_level levels/smoke_level.desc editor/exit_after_level_loaded true \
	app/exit_after_frames 20000 app/headless_target_fps 0 > "${log}.stdout" 2> "${log}.stderr") || rc=$?
tail -30 "${log}.stderr"
if [[ $rc -ne 0 ]]; then echo "::error::editor.exe --headless exited with ${rc} (124 = timed out)"; exit 1; fi
if ! grep -q "level loaded: .*smoke_level.desc" "${log}.stderr"; then
	echo "::error::the packaged editor did not load the startup level"; exit 1
fi
if grep -E '\[(error|critical)\]' "${log}.stderr"; then echo "::error::errors in the packaged editor's log"; exit 1; fi
echo "the package is complete"
