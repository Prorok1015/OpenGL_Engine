#!/usr/bin/env bash
# Fails when an installed snake_sdk CMake config file hard-codes an absolute path (#239).
#
#   tools/ci/check_sdk_abs_paths.sh <sdk prefix>
#
# Every path in <prefix>/lib/cmake must be ${_IMPORT_PREFIX}-relative, a bare library name or a
# target name; a full path such as /usr/lib/x86_64-linux-gnu/librt.so (find_library result linked
# by full path) ties the SDK to the machine it was built on. Generator expressions and
# ${...}-relative paths do not match; paths under the prefix itself are tolerated.
set -euo pipefail

prefix="${1:?usage: check_sdk_abs_paths.sh <sdk prefix>}"
cmake_dir="${prefix}/lib/cmake"
if [[ ! -d "${cmake_dir}" ]]; then
	echo "::error::${cmake_dir} does not exist; is ${prefix} an installed SDK?"
	exit 2
fi

# A path starts after a quote, ';', ':' (generator expression argument), ',', '(' or whitespace,
# and is POSIX-absolute or has a drive letter.
path_re='(^|["(;:,[:space:]])(/[^]"();,[:space:]$<>]+|[A-Za-z]:[/\\][^]"();,[:space:]$<>]*)'

prefix_posix="${prefix}"
if command -v cygpath >/dev/null 2>&1; then
	prefix_posix="$(cygpath -m "${prefix}")"
fi

found=0
checked=0
while IFS= read -r -d '' file; do
	checked=$((checked + 1))
	n=0
	while IFS= read -r line || [[ -n "${line}" ]]; do
		n=$((n + 1))
		trimmed="${line#"${line%%[![:space:]]*}"}"
		[[ "${trimmed}" == \#* ]] && continue
		while IFS= read -r hit; do
			[[ -z "${hit}" ]] && continue
			case "${hit}" in
				[/A-Za-z]*) ;;
				*) hit="${hit#?}" ;;
			esac
			case "${hit}" in
				"${prefix}"/*|"${prefix_posix}"/*) continue ;;
			esac
			echo "::error file=${file#"${prefix}"/},line=${n}::absolute path outside the install prefix: ${hit}"
			echo "  ${file}:${n}: ${trimmed}"
			found=$((found + 1))
		done < <(grep -oE "${path_re}" <<<"${line}" || true)
	done <"${file}"
done < <(find "${cmake_dir}" -type f -name '*.cmake' -print0 | sort -z)

echo "check_sdk_abs_paths: ${checked} files scanned, ${found} absolute path(s) found"
if [[ "${checked}" -eq 0 ]]; then
	echo "::error::no *.cmake files under ${cmake_dir}"
	exit 2
fi
[[ "${found}" -eq 0 ]]
