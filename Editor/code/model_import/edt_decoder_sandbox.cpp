#include "edt_decoder_sandbox.h"
#include <format>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>
#endif

edt::decoder_sandbox_status edt::expected_decoder_sandbox(sandbox_mode mode)
{
	decoder_sandbox_status expected;
	expected.reported = true;
	if (mode == sandbox_mode::off) {
		expected.why = "the OS sandbox is turned off (import/decoder_sandbox)";
		return expected;
	}
#if defined(__linux__)
	constexpr long k_landlock_create_ruleset = 444;	// the same number on x86_64 and aarch64
	constexpr unsigned k_landlock_version = 1u << 0;
	expected.fs = ::syscall(k_landlock_create_ruleset, nullptr, 0ul, k_landlock_version) > 0;
	expected.syscalls = !(::prctl(PR_GET_SECCOMP, 0, 0, 0, 0) < 0 && errno == EINVAL);
	if (!expected.fs) expected.why = "the kernel has no usable Landlock";
	else if (!expected.syscalls) expected.why = "the kernel has no seccomp";
#else
	expected.why = "this system has no sandbox for the decoder yet (Landlock and seccomp are Linux only)";
#endif
	return expected;
}

edt::decoder_sandbox_status edt::unreported_decoder_sandbox()
{
	decoder_sandbox_status unreported;
	unreported.reported = true;
	unreported.why = "the decoder did not say whether it runs in its OS sandbox";
	return unreported;
}

int edt::decoder_sandbox_gap(const decoder_sandbox_status& status)
{
	if (!status.reported) return 0;
	if (!status.fs) return 2;
	return status.syscalls ? 0 : 1;
}

std::string edt::decoder_sandbox_warning(const decoder_sandbox_status& status)
{
	const std::string why = status.why.empty() ? std::string("no reason given") : status.why;
	switch (decoder_sandbox_gap(status)) {
	case 2:
		// seccomp alone leaves open() and write() allowed: without Landlock no file of the user is protected.
		return std::format("The model decoder runs without OS isolation ({}): the parser of a model can read and change every file "
			"this user can. The setting is import/decoder_sandbox.", why);
	case 1:
		return std::format("The model decoder runs without its system call filter ({}). The setting is import/decoder_sandbox.", why);
	default:
		return {};
	}
}
