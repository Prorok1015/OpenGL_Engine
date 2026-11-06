#!/usr/bin/env bash
# Fails when a binary of the Windows package needs a DLL that is neither in the package nor part of Windows (#223).
#
#   tools/ci/check_windows_deps.sh <bin dir> <file>...
#
# Reads the imports (and delay-load imports) with `dumpbin -dependents` (an MSVC developer shell). A dependency is
# fine when it sits in <bin dir> next to the binary, or is a Windows system DLL (an API set, or a file in
# %SystemRoot%\System32). The MSVC runtime (vcruntime*, msvcp*, concrt*, vccorlib*, vcomp*) must be in the package
# even though the runner has it in System32: an end user's machine may not.
set -euo pipefail

bin="${1:?usage: check_windows_deps.sh <bin dir> <file>...}"
shift
[[ $# -gt 0 ]] || { echo "usage: check_windows_deps.sh <bin dir> <file>..."; exit 2; }
sys="$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")/System32"

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
		if [[ -f "${bin}/${dep}" ]]; then
			echo "  ${dep}: in the package"
		elif [[ "$lower" =~ ^(vcruntime|msvcp|concrt|vccorlib|vcomp) ]]; then
			echo "::error::${f} needs ${dep} (MSVC runtime) but it is not in ${bin}"
			status=1
		elif [[ "$lower" == api-ms-win-* || "$lower" == ext-ms-* ]]; then
			echo "  ${dep}: Windows API set"
		elif [[ -f "${sys}/${dep}" ]]; then
			echo "  ${dep}: Windows"
		else
			echo "::error::${f} needs ${dep}, which is neither in ${bin} nor a Windows system DLL"
			status=1
		fi
	done <<< "$deps"
done
exit $status
