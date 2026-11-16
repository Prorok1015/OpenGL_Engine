#pragma once
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

// What the decoder's OS sandbox (#1043) can do on the machine the tests run on.
namespace test_util {

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	inline constexpr bool k_decoder_has_syscall_filter = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	inline constexpr bool k_decoder_has_syscall_filter = false;
#else
	inline constexpr bool k_decoder_has_syscall_filter = true;
#endif
#else
	inline constexpr bool k_decoder_has_syscall_filter = true;
#endif

	// The Landlock ABI of the running kernel, as the decoder probes it; 0 without Landlock (or off Linux).
	inline int landlock_abi()
	{
#if defined(__linux__)
		constexpr long k_create_ruleset = 444;	// __NR_landlock_create_ruleset on x86_64 and aarch64
		constexpr unsigned k_version = 1u << 0;	// LANDLOCK_CREATE_RULESET_VERSION
		const long abi = ::syscall(k_create_ruleset, nullptr, static_cast<unsigned long>(0), k_version);
		return abi > 0 ? static_cast<int>(abi) : 0;
#else
		return 0;
#endif
	}

	// True when there is no Landlock and a sandbox test should stop. On a Linux CI runner (CI set) that is an error, not a
	// skip: the sandbox tests would pass there while testing nothing. Other systems have no sandbox yet (#1054).
	inline bool skipped_without_landlock()
	{
		if (landlock_abi() != 0) return false;
#if defined(__linux__)
		const char* ci = std::getenv("CI");
		if (ci != nullptr && *ci != '\0') {
			BOOST_ERROR("the CI runner has no Landlock: the decoder's sandbox is not tested");
			return true;
		}
#endif
		BOOST_TEST_MESSAGE("skipped: this system has no Landlock");
		return true;
	}

}
