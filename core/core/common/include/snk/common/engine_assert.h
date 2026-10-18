#pragma once
#include <format>
#include <string_view>
#include <snk/common/snk_api.h>

// Assert policy:
//   ASSERT_MSG / ASSERT_FAIL  - internal invariants (programmer bugs). Active when NDEBUG is not
//                               defined (or SNK_FORCE_ASSERTS is defined). Compiled out otherwise;
//                               the condition is NOT evaluated then.
//   ENGINE_VERIFY             - external data / contracts. ALWAYS evaluates the condition, logs on
//                               failure and yields bool so the caller can degrade gracefully.
// The message (after the condition) is a std::format string followed by optional format args.

namespace engine::detail
{
	// Logs "assert" category error with location, then aborts. Defined in engine_assert.cpp.
	[[noreturn]] SNK_API void assert_failed(std::string_view msg, const char* file, int line);
	// Logs "verify" category error with location; returns normally.
	SNK_API void verify_failed(std::string_view msg, const char* file, int line);
}

#if !defined(NDEBUG) || defined(SNK_FORCE_ASSERTS)
#define SNK_ASSERTS_ENABLED 1
#define ASSERT_MSG(cond, ...) \
	(static_cast<bool>(cond) ? (void)0 : ::engine::detail::assert_failed(std::format(__VA_ARGS__), __FILE__, __LINE__))
#define ASSERT_FAIL(...) \
	::engine::detail::assert_failed(std::format(__VA_ARGS__), __FILE__, __LINE__)
#else
#define SNK_ASSERTS_ENABLED 0
#define ASSERT_MSG(cond, ...) ((void)sizeof(static_cast<bool>(cond)))
#define ASSERT_FAIL(...) ((void)0)
#endif

// Evaluates `cond` exactly once in every build type. Logs only (no abort) so tests and release
// builds keep running; callers are expected to handle the false result.
#define ENGINE_VERIFY(cond, ...) \
	(static_cast<bool>(cond) || (::engine::detail::verify_failed(std::format(__VA_ARGS__), __FILE__, __LINE__), false))
