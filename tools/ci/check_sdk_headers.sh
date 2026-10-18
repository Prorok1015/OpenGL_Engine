#!/usr/bin/env bash
# Checks the headers an installed snake_sdk exposes (#109, docs/roadmap/design/HEADER_SPLIT.md).
#
#   tools/ci/check_sdk_headers.sh <sdk prefix> [--enforce | --report]
#
# Engine-internal headers must not reach <prefix>/include: files named impl_*, rnd_gl_*, anything
# under passes/, anything with glad/GLFW in its path, and headers that #include <glad/...> or
# <GLFW/...>. Bundled third-party trees (snk/boost, snk/lib3dparty, snk/_generated) are not checked.
#
# Until the components have moved their public headers to <component>/include (PR-1..PR-4) the
# install still contains such files, so the check only reports them. The PR that migrates the last
# offender sets ENFORCE_DEFAULT=1. SNK_SDK_HEADERS_ENFORCE=0|1 or --enforce/--report override it.
set -euo pipefail

ENFORCE_DEFAULT=0

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
