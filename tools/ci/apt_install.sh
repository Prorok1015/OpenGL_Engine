#!/usr/bin/env bash
# Installs Ubuntu packages on a CI runner without hanging on a mirror that does not answer (#724):
#
#   tools/ci/apt_install.sh <package>...
#
# Every apt call retries its downloads and gives up on a silent connection after a timeout. A failed `apt-get update`
# (security.ubuntu.com or another index unreachable) does not stop the install: the indexes that did load are used,
# and what the runner image already holds is enough for the packages CI asks for. The whole update + install is tried
# up to three times with a growing pause; only a failed install fails the step. While none of the Ubuntu mirrors
# resolves, an attempt skips apt: apt would spend its whole 900 s on 20 s DNS timeouts, package by package.
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

ubuntu_mirror_hosts() {
	cat /etc/apt/sources.list /etc/apt/sources.list.d/* /etc/apt/apt-mirrors.txt 2>/dev/null \
		| grep -vE '^[[:space:]]*#' | grep -oE '(https?|mirror\+file)://[^/[:space:]]+' | sed -E 's#^[a-z+]+://##' \
		| grep -E 'ubuntu\.com$' | sort -u
}

any_host_resolves() {
	local host
	for host in "$@"; do
		if timeout 10 getent hosts "${host}" >/dev/null; then
			return 0
		fi
	done
	return 1
}

pause_before_next_attempt() {
	local attempt=$1
	if [[ ${attempt} -lt ${attempts} ]]; then
		local pause=$((attempt * 30))
		echo "apt: next attempt in ${pause} s"
		sleep "${pause}"
	fi
}

mapfile -t mirror_hosts < <(ubuntu_mirror_hosts)

for attempt in $(seq 1 "${attempts}"); do
	if [[ ${#mirror_hosts[@]} -gt 0 ]] && ! any_host_resolves "${mirror_hosts[@]}"; then
		echo "::warning title=apt::attempt ${attempt}/${attempts}: DNS does not resolve any Ubuntu mirror (${mirror_hosts[*]}), apt skipped"
		pause_before_next_attempt "${attempt}"
		continue
	fi

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
	pause_before_next_attempt "${attempt}"
done

echo "::error title=apt::the packages could not be installed after ${attempts} attempts: $*"
exit 1
