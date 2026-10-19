#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace engine
{
	enum class log_level { trace, debug, info, warn, error };

	// Initialize the logging system. Call once at startup.
	SNK_API void init_logger();

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
	// block. It may log itself or remove its own handle.
	using log_callback_t = std::function<void(std::string_view, log_level, std::string_view)>;

	SNK_API void remove_log_sink(std::uint64_t id);

	// Move-only RAII handle of one sink: the sink is removed when the handle is destroyed or reset. After reset()
	// returns, the sink is not running and is never called again (a call in flight on another thread is waited
	// for). Harmless after shutdown_logger().
	class log_sink_handle
	{
	public:
		log_sink_handle() = default;
		explicit log_sink_handle(std::uint64_t id) noexcept : m_id(id) {}
		log_sink_handle(const log_sink_handle&) = delete;
		log_sink_handle& operator=(const log_sink_handle&) = delete;
		log_sink_handle(log_sink_handle&& other) noexcept : m_id(other.m_id) { other.m_id = 0; }
		log_sink_handle& operator=(log_sink_handle&& other) noexcept
		{
			if (this != &other) {
				reset();
				m_id = other.m_id;
				other.m_id = 0;
			}
			return *this;
		}
		~log_sink_handle() { reset(); }

		bool active() const noexcept { return m_id != 0; }
		void reset()
		{
			if (m_id != 0) {
				remove_log_sink(m_id);
				m_id = 0;
			}
		}

	private:
		std::uint64_t m_id = 0;
	};

	// Any number of sinks may be installed at once; they are called in the order they were added.
	// A null callback adds nothing (the handle is inactive).
	[[nodiscard]] SNK_API log_sink_handle add_log_sink(log_callback_t callback);

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
