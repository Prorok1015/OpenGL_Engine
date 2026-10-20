#!/usr/bin/env bash
# Checks that the MCP server module (snk_mcp_server) stays out of the engine, the games and the SDK, and that it
# stays free of the engine (docs/roadmap/design/AI_INTEGRATION.md section 3.7, decision 7).
#
#   tools/ci/check_mcp_isolation.sh [repo root]          the sources git tracks
#   tools/ci/check_mcp_isolation.sh --sdk <sdk prefix>   an installed SDK (cmake --install of the snake_sdk package)
#
# Source check:
#   1. CMake: only the files in ALLOW_CMAKE may name the module (mcp_server). Nothing under core/, no tool, no game and no
#      consumer example links it. The list is tight in both directions, like check_no_gl_outside_drivers.sh: a file outside
#      it with a hit fails, and an entry without a hit fails, so the list says exactly where the module is wired in.
#   2. C++: #include <snk/mcp/...> only under Editor/code/mcp_server (the module), Editor/code/editor_system (the adapter)
#      and tests/.
#   3. The module includes no other project header and uses no engine facility (egLOG, ASSERT_MSG, ds::, cmd::, ...),
#      and its CMake links nothing of the project except the build-flag targets.
# SDK check: no file named after the module, no snk/mcp headers, no mention in the exported CMake package.
#
# cmake/SnkMcpIsolation.cmake is the twin on the CMake graph: it fails the configure if a target under core/ or the SDK
# closure reaches the module through its link interface.
set -euo pipefail

# 'path|reason'
ALLOW_CMAKE=(
	'CMakeLists.txt|links the module into unit_tests (its own tests) and calls snk_check_mcp_isolation'
	'cmake/SnkMcpIsolation.cmake|the check on the CMake graph'
	'Editor/CMakeLists.txt|adds the module next to editor_system'
	'Editor/code/editor_system/CMakeLists.txt|the editor adapter links the module'
	'Editor/code/mcp_server/CMakeLists.txt|the module itself'
)
# Directories (prefixes) whose C++ may include <snk/mcp/...>.
ALLOW_INCLUDE=(
	'Editor/code/mcp_server/'
	'Editor/code/editor_system/'
	'tests/'
)
MODULE_DIR='Editor/code/mcp_server'

if [[ "${1:-}" == "--sdk" ]]; then
	prefix="${2:?usage: check_mcp_isolation.sh --sdk <sdk prefix>}"
	if [[ ! -d "${prefix}/include/snk" ]]; then
		echo "::error::${prefix}/include/snk does not exist; is ${prefix} an installed SDK?"
		exit 2
	fi
	found=()
	while IFS= read -r path; do
		if [[ -n "${path}" ]]; then found+=("${path}"); fi
	done < <(find "${prefix}" \( -iname '*mcp_server*' -o -path '*/include/snk/mcp' -o -path '*/include/snk/mcp/*' \) | LC_ALL=C sort)
	if [[ -d "${prefix}/lib/cmake" ]]; then
		while IFS= read -r path; do
			if [[ -n "${path}" ]]; then found+=("${path}  (mentions the module)"); fi
		done < <(grep -rIil 'mcp_server' "${prefix}/lib/cmake" | LC_ALL=C sort || true)
	fi
	echo "SDK ${prefix}: ${#found[@]} file(s) of the MCP server module"
	for entry in ${found[@]+"${found[@]}"}; do
		echo "  ${entry}"
	done
	if ((${#found[@]} > 0)); then
		echo "::error::the SDK contains the MCP server module (snk_mcp_server); it belongs to the editor only"
		exit 1
	fi
	exit 0
fi

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "${root}"
status=0

# ---- 1. CMake ---------------------------------------------------------------------------------------------------
declare -A cmake_hits=()
for entry in "${ALLOW_CMAKE[@]}"; do
	cmake_hits["${entry%%|*}"]=0
done
outside=()
while IFS= read -r file; do
	[[ -n "${file}" ]] || continue
	if [[ -n "${cmake_hits[${file}]+set}" ]]; then
		cmake_hits["${file}"]=$((cmake_hits["${file}"] + 1))
	else
		outside+=("${file}")
	fi
done < <(git grep -lI 'mcp_server' -- '*CMakeLists.txt' '*.cmake' '*.cmake.in' || true)

echo "CMake files that name the MCP server module:"
for entry in "${ALLOW_CMAKE[@]}"; do
	printf '  allowed  %s\n           %s\n' "${entry%%|*}" "${entry#*|}"
done
if ((${#outside[@]} > 0)); then
	echo "::error::the MCP server module is named outside the allow-list of tools/ci/check_mcp_isolation.sh (engine, game and SDK targets must not link it)"
	for file in "${outside[@]}"; do
		echo "  ${file}"
	done
	status=1
fi
for entry in "${ALLOW_CMAKE[@]}"; do
	file="${entry%%|*}"
	if ((cmake_hits[${file}] == 0)); then
		echo "::error::allow-list entry '${file}' no longer names the module; remove it from tools/ci/check_mcp_isolation.sh"
		status=1
	fi
done

# ---- 2. C++ includes --------------------------------------------------------------------------------------------
include_re='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]snk/mcp/'
bad_includes=()
while IFS= read -r line; do
	[[ -n "${line}" ]] || continue
	path="${line%%:*}"
	allowed=0
	for prefix in "${ALLOW_INCLUDE[@]}"; do
		if [[ "${path}" == "${prefix}"* ]]; then allowed=1; fi
	done
	((allowed)) || bad_includes+=("${line}")
done < <(git grep -nIE "${include_re}" -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' '*.inl' '*.ipp' '*.mm' ":(exclude)lib3dparty" || true)
if ((${#bad_includes[@]} > 0)); then
	echo "::error::${#bad_includes[@]} include(s) of <snk/mcp/...> outside the editor and the tests"
	for line in "${bad_includes[@]}"; do
		echo "  ${line}"
	done
	status=1
fi

# ---- 3. The module is free of the engine ------------------------------------------------------------------------
foreign=()
while IFS= read -r line; do
	if [[ -n "${line}" ]]; then foreign+=("${line}"); fi
done < <(git grep -nIE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]snk/' -- "${MODULE_DIR}" | grep -vE '[<"]snk/mcp/' || true)
while IFS= read -r line; do
	if [[ -n "${line}" ]]; then foreign+=("${line}"); fi
done < <(git grep -nIE '\b(egLOG[A-Za-z_]*|ASSERT_MSG|ASSERT_FAIL|ENGINE_VERIFY)\b|\b(ds|cmd|res|rnd|scn|ecs|edt|engine)::' -- "${MODULE_DIR}/*.cpp" "${MODULE_DIR}/*.h" || true)
if ((${#foreign[@]} > 0)); then
	echo "::error::${#foreign[@]} use(s) of the engine inside ${MODULE_DIR} (the module depends on Boost only)"
	for line in "${foreign[@]}"; do
		echo "  ${line}"
	done
	status=1
fi
linked=()
while IFS= read -r name; do
	[[ -n "${name}" ]] || continue
	case "${name}" in
		snk::mcp_server | snk::sanitizers | snk::compiler_warnings) ;;
		*) linked+=("${name}") ;;
	esac
done < <(grep -ohE 'snk::[A-Za-z0-9_]+' "${MODULE_DIR}/CMakeLists.txt" | LC_ALL=C sort -u || true)
if ((${#linked[@]} > 0)); then
	echo "::error::${MODULE_DIR}/CMakeLists.txt names project target(s) besides the build-flag ones: ${linked[*]}"
	status=1
fi

if ((status == 0)); then
	echo "MCP server isolation: ok"
fi
exit "${status}"
