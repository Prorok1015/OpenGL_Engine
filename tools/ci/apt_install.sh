#!/usr/bin/env bash
# Installs Ubuntu packages on a CI runner without hanging on a mirror that does not answer (#724):
#
#   tools/ci/apt_install.sh <package>...
#
# Every apt call retries its downloads and gives up on a silent connection after a timeout. A failed `apt-get update`
# (security.ubuntu.com or another index unreachable) does not stop the install: the indexes that did load are used,
# and what the runner image already holds is enough for the packages CI asks for. The whole update + install is tried
# up to three times with a growing pause; only a failed install fails the step.
set -uo pipefail

if [[ $# -eq 0 ]]; then
	echo "usage: apt_install.sh <package>..." >&2
	exit 2
fi

apt_options=(
	-o Acquire::Retries=3
	-o Acquire::http::Timeout=20
	-o Acquire::https::Timeout=20
	-o Acquire::ftp::Timeout=20
	-o DPkg::Lock::Timeout=120
)
attempts=3

for attempt in $(seq 1 "${attempts}"); do
	echo "::group::apt-get update (attempt ${attempt}/${attempts})"
	update_rc=0
	sudo timeout 300 apt-get "${apt_options[@]}" update || update_rc=$?
	echo "::endgroup::"
	if [[ ${update_rc} -ne 0 ]]; then
		echo "::warning title=apt::apt-get update exited with ${update_rc} (an index could not be fetched; 124 = timed out after 300 s): installing from the indexes that loaded"
	fi

	echo "::group::apt-get install $* (attempt ${attempt}/${attempts})"
	install_rc=0
	sudo timeout 900 apt-get "${apt_options[@]}" install -y --no-install-recommends "$@" || install_rc=$?
	echo "::endgroup::"
	if [[ ${install_rc} -eq 0 ]]; then
		echo "apt: installed $* (attempt ${attempt})"
		exit 0
	fi

	echo "::warning title=apt::apt-get install exited with ${install_rc} on attempt ${attempt}/${attempts} (124 = timed out after 900 s)"
	if [[ ${attempt} -lt ${attempts} ]]; then
		pause=$((attempt * 30))
		echo "apt: next attempt in ${pause} s"
		sleep "${pause}"
	fi
done

echo "::error title=apt::the packages could not be installed after ${attempts} attempts: $*"
exit 1
