#!/usr/bin/env bash
# Checks the headers an installed snake_sdk exposes (#109, docs/roadmap/design/HEADER_SPLIT.md).
#
#   tools/ci/check_sdk_headers.sh <sdk prefix> [--enforce | --report]
#
# Engine-internal headers must not reach <prefix>/include: files named impl_*, rnd_gl_*, anything
# under passes/, anything with glad/GLFW in its path, and headers that #include <glad/...> or
# <GLFW/...>. Bundled third-party trees (snk/boost, snk/lib3dparty, snk/_generated) are not checked.
#
# Base-only API (<component>/include_base, #405 / MODULE_LAYER.md) must not reach the SDK either: no file of it, no
# installed header that includes it or declares what it holds. Always enforced.
#
# Enforcing by default since every public header lives in <component>/include (#109).
# SNK_SDK_HEADERS_ENFORCE=0|1 or --enforce/--report override it.
set -euo pipefail

ENFORCE_DEFAULT=1

# Extended regexes (matched against the path relative to <prefix>/include) that stay tolerated even
# when enforcing. Keep empty; every entry needs an issue reference.
ALLOW=()

prefix="${1:?usage: check_sdk_headers.sh <sdk prefix> [--enforce|--report]}"
enforce="${SNK_SDK_HEADERS_ENFORCE:-${ENFORCE_DEFAULT}}"
case "${2:-}" in
	--enforce) enforce=1 ;;
	--report) enforce=0 ;;
	"") ;;
	*) echo "unknown option: $2" >&2; exit 2 ;;
esac

root="${prefix}/include"
if [[ ! -d "${root}/snk" ]]; then
	echo "::error::${root}/snk does not exist; is ${prefix} an installed SDK?"
	exit 2
fi

path_re='(^|/)(impl_[^/]*|rnd_gl_[^/]*|passes|[^/]*glad[^/]*|[^/]*GLFW[^/]*)(/|$)'
include_re='^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](glad|GLFW)/'
third_party_re='^snk/(boost|lib3dparty|_generated)/'

found=()
while IFS= read -r rel; do
	[[ "${rel}" =~ ${third_party_re} ]] && continue
	if [[ "${rel}" =~ ${path_re} ]]; then
		found+=("${rel}  (path)")
	elif grep -qE "${include_re}" "${root}/${rel}"; then
		found+=("${rel}  (includes glad/GLFW)")
	fi
done < <(cd "${root}" && find snk -type f | LC_ALL=C sort)

would_fail=()
for entry in ${found[@]+"${found[@]}"}; do
	rel="${entry%%  (*}"
	allowed=0
	for re in ${ALLOW[@]+"${ALLOW[@]}"}; do
		[[ "${rel}" =~ ${re} ]] && allowed=1
	done
	((allowed)) || would_fail+=("${entry}")
done

# Base-only API: the names come from the include_base trees; the self-test proves the check still catches a leak.
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
python="$(command -v python3 || command -v python)" # Windows runners have only `python`
"${python}" "${here}/check_sdk_base_only.py" --self-test
"${python}" "${here}/check_sdk_base_only.py" "${prefix}"

total=$(cd "${root}" && find snk -type f | wc -l)
echo "SDK headers: ${total} files installed under include/snk, ${#would_fail[@]} internal"
for entry in ${would_fail[@]+"${would_fail[@]}"}; do
	echo "  internal: ${entry}"
done

if ((${#would_fail[@]} == 0)); then
	exit 0
fi
if [[ "${enforce}" == "1" ]]; then
	echo "::error::${#would_fail[@]} engine-internal header(s) are installed in the SDK"
	exit 1
fi
echo "::warning::${#would_fail[@]} engine-internal header(s) installed in the SDK (report-only, enforcement is off)"
exit 0
