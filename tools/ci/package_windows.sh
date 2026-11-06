#!/usr/bin/env bash
# Builds one configuration of the Windows release package (#223), from two builds of the engine:
#
#   tools/ci/package_windows.sh <Release|Debug> <output dir> <version>
#
# Release -> <out>/Snake-<version>-windows-x64.zip: Snake/<version>/ with the editor, the SDK with both
#            engines (snk::engine = snk_engine.dll, snk::engine_static), PDBs and the MSVC runtime.
# Debug   -> <out>/Snake-<version>-windows-x64-debug.zip: only what a Debug build adds (snk_engined.dll,
#            *d.lib, *d.pdb, snake_sdk_*Targets-debug.cmake), in the same layout: it is unpacked over the
#            Release package and overwrites nothing.
#
# Run from the repository root in an MSVC developer shell (cl, ninja and 7z on PATH). Extra configure
# arguments come from SNK_PACKAGE_CMAKE_ARGS (release.yml: -DSNK_ASSIMP_FORMATS=ALL), the FetchContent cache
# from FC_DIR. Uses the presets ci-windows-msvc-{static-,}{release,debug}.
set -euo pipefail

config="${1:?usage: package_windows.sh <Release|Debug> <output dir> <version>}"
out="${2:?usage: package_windows.sh <Release|Debug> <output dir> <version>}"
ver="${3:?usage: package_windows.sh <Release|Debug> <output dir> <version>}"
case "$config" in
	Release) cfg=release; zip_suffix="" ;;
	Debug) cfg=debug; zip_suffix="-debug" ;;
	*) echo "::error::configuration must be Release or Debug, not '$config'"; exit 2 ;;
esac
if command -v cygpath >/dev/null; then out="$(cygpath -m "$out")"; fi

static_preset="ci-windows-msvc-static-${cfg}"
shared_preset="ci-windows-msvc-${cfg}"
read -r -a extra <<< "${SNK_PACKAGE_CMAKE_ARGS:-}"
if [[ -n "${FC_DIR:-}" ]]; then extra+=("-DFETCHCONTENT_BASE_DIR=${FC_DIR}"); fi

for preset in "$static_preset" "$shared_preset"; do
	cmake --preset "$preset" "${extra[@]}"
done
cmake --build "build/${static_preset}" --target snk_sdk --parallel
if [[ "$config" == Release ]]; then
	cmake --build "build/${shared_preset}" --target snk_sdk editor --parallel
else
	cmake --build "build/${shared_preset}" --target snk_sdk --parallel
fi

stage="${out}/stage-${cfg}"
rm -rf "$stage"
cmake --install "build/${static_preset}" --prefix "${stage}/static" --component snake_sdk
cmake --install "build/${shared_preset}" --prefix "${stage}/shared" --component snake_sdk
bash tools/ci/check_sdk_merge.sh "${stage}/static" "${stage}/shared"

# The package prefix: static first, then shared (both SDK halves), then the editor.
full="${stage}/full"
cmake --install "build/${static_preset}" --prefix "$full" --component snake_sdk
cmake --install "build/${shared_preset}" --prefix "$full" --component snake_sdk
bash tools/ci/check_sdk_headers.sh "$full"
bash tools/ci/check_sdk_abs_paths.sh "$full"
# The SDK alone, before the editor (which links the MCP server module) joins the prefix.
bash tools/ci/check_mcp_isolation.sh --sdk "$full"
ws="${GITHUB_WORKSPACE:-$PWD}"
if command -v cygpath >/dev/null; then ws="$(cygpath -m "$ws")"; fi
if grep -rIn -e "$ws" "${full}/lib/cmake"; then
	echo "::error::the installed package config references the workspace"
	exit 1
fi
if [[ "$config" == Release ]]; then
	cmake --install "build/${shared_preset}" --prefix "$full" --component editor
fi

pkg="${out}/pkg-${cfg}"
root="${pkg}/Snake/${ver}"
rm -rf "$pkg"
mkdir -p "$root"
if [[ "$config" == Release ]]; then
	cp -a "${full}/." "${root}/"
else
	# What Debug adds to the Release package, nothing else: the headers, the package config and the
	# configuration-independent targets files come from the Release package.
	status=0
	while IFS= read -r f; do
		case "$f" in
			*.cmake) ;;
			*)
				base="$(basename "$f")"
				if [[ "${base%.*}" != *d ]]; then
					echo "::error::${f} has no Debug postfix: unpacked over the Release package it would replace its namesake"
					status=1
				fi
				;;
		esac
		mkdir -p "${root}/$(dirname "$f")"
		cp -a "${full}/${f}" "${root}/${f}"
	done < <(cd "$full" && find bin lib -type f \( -name '*.dll' -o -name '*.pdb' -o -name '*.lib' -o -name '*-debug.cmake' \) | sort)
	[[ $status -eq 0 ]] || exit 1
fi

zip="${out}/Snake-${ver}-windows-x64${zip_suffix}.zip"
rm -f "$zip"
(cd "$pkg" && 7z a -tzip -mx=5 "$zip" Snake > /dev/null)
echo "${zip}:"
(cd "$pkg" && find Snake -type f | sort | sed 's/^/  /' | head -n 400)
ls -l "$zip"
