#!/usr/bin/env bash
# Checks that the editor's MCP server module stays out of the engine, the game and the SDK
# (AI_INTEGRATION step A6a, docs/roadmap/design/AI_INTEGRATION.md section 3.7, decision 7).
#
#   tools/ci/check_mcp_server_isolation.sh [--sdk <installed sdk prefix>] [repo root]
#
# What it enforces:
#   1. Only the files in ALLOW name the CMake target snk_mcp_server / snk::mcp_server: the module's own CMake file, the
#      editor's CMake files that add and link it, and the root CMake file (unit_tests, and the configure-time check
#      that the engine and SDK roots do not reach it). Nothing under core/, cmake/ or tools/ may.
#   2. Only code under Editor/ and tests/ includes <mcp/...>.
#   3. The module's own sources include nothing but the standard library, Boost, platform headers and themselves:
#      no snk/, entt, glm, spdlog, imgui.
#   4. The module's CMake file links nothing but Boost, Threads and the build-wide sanitizer/warning targets.
#   5. With --sdk: the installed SDK has no file named after the module, no <mcp/...> headers and no CMake file that
#      mentions it.
#
# Like check_no_gl_outside_drivers.sh the allow-list is tight in both directions: a hit outside it fails the check, and
# an entry without a hit fails it too.
set -euo pipefail

MODULE_DIR='Editor/code/mcp_server'
TARGET_RE='snk_mcp_server|snk::mcp_server'

# 'path|reason'
ALLOW=(
	'CMakeLists.txt|unit_tests links the module for its tests; the configure-time check keeps snk::engine and snk::engine_api away from it'
	'Editor/CMakeLists.txt|adds the module to the editor build'
	'Editor/code/mcp_server/CMakeLists.txt|defines the module'
	'Editor/code/editor_system/CMakeLists.txt|the editor adapter (edt::mcp_bridge_host) links it'
)

# Link targets the module may use. Everything else with a "::" (snk::common, snk::engine, ...) fails.
ALLOWED_LINK_TARGETS=(
	'Boost::json' 'Boost::asio' 'Boost::beast' 'Threads::Threads'
	'snk::sanitizers' 'snk::compiler_warnings' 'snk::mcp_server'
)

sdk_prefix=''
if [[ "${1:-}" == '--sdk' ]]; then
	sdk_prefix="${2:?usage: check_mcp_server_isolation.sh [--sdk <prefix>] [repo root]}"
	shift 2
fi

root="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "${root}"

status=0
fail() {
	echo "::error::$*"
	status=1
}

# ---- 1. who names the target --------------------------------------------------------------------------------------
declare -A allow_hits=()
for entry in "${ALLOW[@]}"; do
	allow_hits["${entry%%|*}"]=0
done

hits="$(git grep -nIE "${TARGET_RE}" -- \
	":(exclude)docs" ":(exclude).github" ":(exclude)lib3dparty" ":(exclude)tools/ci/check_mcp_server_isolation.sh" || true)"
outside=()
while IFS= read -r line; do
	[[ -n "${line}" ]] || continue
	path="${line%%:*}"
	if [[ -n "${allow_hits[${path}]+x}" ]]; then
		allow_hits["${path}"]=$((allow_hits["${path}"] + 1))
	else
		outside+=("${line}")
	fi
done <<<"${hits}"

echo "snk_mcp_server named in:"
for entry in "${ALLOW[@]}"; do
	path="${entry%%|*}"
	printf '  allowed  %3d hit(s)  %s\n           %s\n' "${allow_hits[${path}]}" "${path}" "${entry#*|}"
	if ((allow_hits[${path}] == 0)); then
		fail "allow-list entry '${path}' no longer names snk_mcp_server; remove it from tools/ci/check_mcp_server_isolation.sh"
	fi
done
if ((${#outside[@]} > 0)); then
	fail "${#outside[@]} reference(s) to snk_mcp_server outside the allow-list: only the editor may link the MCP server"
	for line in "${outside[@]}"; do
		echo "  ${line}"
	done
fi

# ---- 2. who includes the module's headers ---------------------------------------------------------------------------
include_hits="$(git grep -nIE '#[[:space:]]*include[[:space:]]*[<"]mcp/' -- \
	'*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' '*.inl' ":(exclude)lib3dparty" || true)"
bad_includes=()
while IFS= read -r line; do
	[[ -n "${line}" ]] || continue
	path="${line%%:*}"
	if [[ "${path}" != Editor/* && "${path}" != tests/* ]]; then
		bad_includes+=("${line}")
	fi
done <<<"${include_hits}"
if ((${#bad_includes[@]} > 0)); then
	fail "${#bad_includes[@]} include(s) of <mcp/...> outside Editor/ and tests/"
	for line in "${bad_includes[@]}"; do
		echo "  ${line}"
	done
fi

# ---- 3. what the module includes ------------------------------------------------------------------------------------
echo "Includes of ${MODULE_DIR}:"
module_files="$(git ls-files "${MODULE_DIR}" | grep -E '\.(cpp|h|hpp)$' || true)"
if [[ -z "${module_files}" ]]; then
	fail "${MODULE_DIR} has no sources"
fi
engine_includes=()
while IFS= read -r file; do
	[[ -n "${file}" ]] || continue
	while IFS= read -r inc; do
		[[ -n "${inc}" ]] || continue
		# '<name>' or '"name"' without a directory: the standard library or a header of the module.
		if [[ "${inc}" != */* ]]; then continue; fi
		case "${inc}" in
			mcp/* | boost/* | sys/*) ;;
			*) engine_includes+=("${file}: #include ${inc}") ;;
		esac
	done < <(sed -nE 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^>"]+)[>"].*/\1/p' "${file}")
done <<<"${module_files}"
if ((${#engine_includes[@]} > 0)); then
	fail "${#engine_includes[@]} include(s) in ${MODULE_DIR} that are not standard library, Boost or the module's own"
	for line in "${engine_includes[@]}"; do
		echo "  ${line}"
	done
else
	echo "  only the standard library, Boost, platform headers and mcp/"
fi

# ---- 4. what the module links ---------------------------------------------------------------------------------------
echo "Link targets of ${MODULE_DIR}/CMakeLists.txt:"
bad_links=()
while IFS= read -r token; do
	[[ -n "${token}" ]] || continue
	known=0
	for allowed in "${ALLOWED_LINK_TARGETS[@]}"; do
		[[ "${token}" == "${allowed}" ]] && known=1
	done
	((known)) || bad_links+=("${token}")
done < <(grep -v '^[[:space:]]*#' "${MODULE_DIR}/CMakeLists.txt" | grep -oE '[A-Za-z0-9_]+::[A-Za-z0-9_]+' | sort -u)
if grep -v '^[[:space:]]*#' "${MODULE_DIR}/CMakeLists.txt" | grep -qE '(ENGINE_CORE_PATH|add_subdirectory|\.\./)'; then
	fail "${MODULE_DIR}/CMakeLists.txt reaches outside the module (ENGINE_CORE_PATH, add_subdirectory or ../)"
fi
if ((${#bad_links[@]} > 0)); then
	fail "${MODULE_DIR}/CMakeLists.txt links targets other than Boost, Threads and the build flags: ${bad_links[*]}"
else
	echo "  Boost, Threads and the sanitizer/warning flags only"
fi

# ---- 5. the installed SDK -------------------------------------------------------------------------------------------
if [[ -n "${sdk_prefix}" ]]; then
	echo "Installed SDK ${sdk_prefix}:"
	if [[ ! -d "${sdk_prefix}" ]]; then
		fail "${sdk_prefix} is not a directory; is it an installed SDK?"
	else
		sdk_hits=()
		while IFS= read -r found; do
			[[ -n "${found}" ]] && sdk_hits+=("${found}  (file name)")
		done < <(find "${sdk_prefix}" \( -iname '*mcp_server*' -o -path '*/include/mcp' -o -path '*/include/mcp/*' -o -path '*/include/snk/mcp' -o -path '*/include/snk/mcp/*' \) | LC_ALL=C sort)
		while IFS= read -r found; do
			[[ -n "${found}" ]] && sdk_hits+=("${found}  (mentions snk_mcp_server)")
		done < <(grep -rIlE "${TARGET_RE}" "${sdk_prefix}" 2>/dev/null | LC_ALL=C sort || true)
		if ((${#sdk_hits[@]} > 0)); then
			fail "${#sdk_hits[@]} MCP server file(s) in the installed SDK"
			for line in "${sdk_hits[@]}"; do
				echo "  ${line}"
			done
		else
			echo "  no MCP server file, header or CMake mention"
		fi
	fi
fi

exit "${status}"
