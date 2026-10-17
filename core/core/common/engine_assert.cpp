#include "engine_assert.h"

#include "engine_log.h"

#include <cstdio>
#include <cstdlib>

namespace engine::detail
{
	void assert_failed(std::string_view msg, const char* file, int line)
	{
		egLOG_ERROR("assert", "{} at {}:{}", msg, file, line);
		// The logger may be uninitialised or buffered; make sure the message survives the abort.
		std::fprintf(stderr, "ASSERT FAILED: %.*s at %s:%d\n", static_cast<int>(msg.size()), msg.data(), file, line);
		std::fflush(stderr);
		std::abort();
	}

	void verify_failed(std::string_view msg, const char* file, int line)
	{
		egLOG_ERROR("verify", "{} at {}:{}", msg, file, line);
	}
}
