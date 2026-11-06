#!/usr/bin/env bash
# Fails when two SDK installs cannot share one prefix (#223).
#
#   tools/ci/check_sdk_merge.sh <prefix A> <prefix B>
#
# The static and the shared engine (or the Debug and the Release build) are installed into one prefix,
# one after the other. That is only sound if the second install overwrites nothing of the first but
# files that are the same in both: the headers and the package config (snake_sdkConfig*.cmake). Any
# other file both installs write (an archive, a targets file) would silently replace the first one.
set -euo pipefail

a="${1:?usage: check_sdk_merge.sh <prefix A> <prefix B>}"
b="${2:?usage: check_sdk_merge.sh <prefix A> <prefix B>}"
for p in "$a" "$b"; do
	if [[ ! -d "$p/lib/cmake/snake_sdk" ]]; then
		echo "::error::$p/lib/cmake/snake_sdk does not exist; is $p an installed SDK?"
		exit 2
	fi
done

list() { (cd "$1" && find . -type f | sed 's|^\./||' | sort); }
common=$(comm -12 <(list "$a") <(list "$b"))

status=0
shared_ok=0
while IFS= read -r f; do
	[[ -z "$f" ]] && continue
	case "$f" in
		include/*|lib/cmake/snake_sdk/snake_sdkConfig.cmake|lib/cmake/snake_sdk/snake_sdkConfigVersion.cmake)
			if ! cmp -s "$a/$f" "$b/$f"; then
				echo "::error::$f differs between the two installs"
				status=1
			fi
			shared_ok=$((shared_ok + 1))
			;;
		*)
			echo "::error::$f is installed by both builds: the second install would replace it"
			status=1
			;;
	esac
done <<< "$common"

echo "files written by both installs: ${shared_ok} (headers and package config, identical)"
exit $status
