#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace engine
{
	enum class log_level { trace, debug, info, warn, error };

	// Where the colored console sink writes. stderr_ keeps stdout free for a protocol (--headless, --mcp-stdio).
	enum class console_target { stdout_, stderr_ };

	// Initialize the logging system. Call once at startup. The first initialization wins; the target is
	// remembered, so a logger re-created after shutdown_logger() writes to the same stream.
	SNK_API void init_logger();
	SNK_API void init_logger(console_target console);

	// Shutdown the logging system. Call once at exit.
	SNK_API void shutdown_logger();

	// Log a pre-formatted message. spdlog is hidden in the .cpp.
	SNK_API void log_message(std::string_view category, log_level level, std::string_view message);

	// Set global log level at runtime.
	SNK_API void set_log_level(log_level level);

	// Set log level for a specific category. Overrides the global level.
	SNK_API void set_category_level(std::string_view category, log_level level);

	// Set the global log file path. Replaces the default "logs/engine.log".
	// Call before init_logger() or at any time — existing loggers are updated.
	void set_log_file(std::string_view path);

	// Add a dedicated file sink for a specific category.
	// Messages from this category will go to both the global sinks and this file.
	SNK_API void set_category_file(std::string_view category, std::string_view path);

	// A log listener (editor console, tests, ...). Signature: (category, level, message). It sees every message,
	// whatever the sink levels say, and runs on the thread that logged, so it must be thread-safe and must not
	// block. From inside its callback it may remove its own handle only; removing another sink there takes effect at
	// once but does not wait for that sink's call in flight on another thread (waiting could lock-cycle). It may log too, but a message logged on a thread that is inside a listener
	// goes to the journal and the log files only, never to a listener (no recursion, no lock-order cycles between
	// listeners). Installing one is base-only (<snk/common/logger/engine_log_sinks.h>); a game module installs one
	// through gs::module_context::add_log_sink.
	using log_callback_t = std::function<void(std::string_view, log_level, std::string_view)>;

	// ---- Journal -------------------------------------------------------------------------------------------
	// Every logged message also goes into a ring buffer, so a reader that was not listening (an agent, a bug
	// report) can ask what happened. Thread-safe; the journal never logs itself.

	inline constexpr std::size_t default_log_journal_capacity = 10000;

	struct log_entry
	{
		// Monotonic, starts at 1, never reused (also across shutdown_logger()).
		std::uint64_t seq = 0;
		std::chrono::system_clock::time_point time;
		log_level level = log_level::info;
		std::string category;
		std::string text;
	};

	struct log_query
	{
		// First sequence number wanted (inclusive). 0: from the oldest entry still held.
		std::uint64_t since_seq = 0;
		log_level min_level = log_level::trace;
		// Empty: every category. Otherwise the category itself or one below it ("edt" matches "edt" and "edt/import").
		std::string category;
		// At least 1.
		std::size_t limit = 100;
	};

	struct log_page
	{
		std::vector<log_entry> entries;
		// Pass it as since_seq to continue exactly after this page.
		std::uint64_t next_seq = 1;
		// since_seq named entries that were already pushed out of the ring.
		bool dropped = false;
	};

	// The logger's global level (what set_log_level last set, or the build's default).
	SNK_API log_level current_log_level();

	// Which messages the journal keeps. Nullopt (the default): the ones the logger itself lets through, i.e. its
	// global and per-category levels; a level here replaces that rule (trace keeps everything). Listeners are not
	// affected: they always see every message.
	SNK_API void set_log_journal_min_level(std::optional<log_level> level);
	SNK_API std::optional<log_level> log_journal_min_level();

	// Keeps the newest `entries` messages (0 turns the journal off). Shrinking drops the oldest at once.
	SNK_API void set_log_journal_capacity(std::size_t entries);
	SNK_API std::size_t log_journal_capacity();
	SNK_API log_page read_log_journal(const log_query& query);
	// The sequence number the next message will get.
	SNK_API std::uint64_t log_journal_next_seq();

} // namespace engine
namespace eg = engine;

// ─── Level macros ────────────────────────────────────────────────────────────
// Each macro: first arg = category string, then format + args.
// Category becomes the spdlog logger name (e.g. "edt/import", "resource").

#define egLOG_TRACE(category, ...) \
	::engine::log_message(category, ::engine::log_level::trace, std::format(__VA_ARGS__))

#define egLOG_DEBUG(category, ...) \
	::engine::log_message(category, ::engine::log_level::debug, std::format(__VA_ARGS__))

#define egLOG_INFO(category, ...) \
	::engine::log_message(category, ::engine::log_level::info, std::format(__VA_ARGS__))

#define egLOG_WARN(category, ...) \
	::engine::log_message(category, ::engine::log_level::warn, std::format(__VA_ARGS__))

#define egLOG_ERROR(category, ...) \
	::engine::log_message(category, ::engine::log_level::error, std::format(__VA_ARGS__))

// Backward-compatible: egLOG maps to info level
#define egLOG(category, ...) egLOG_INFO(category, __VA_ARGS__)
