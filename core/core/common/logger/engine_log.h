#pragma once

#include <string_view>
#include <format>
#include <functional>

namespace engine
{
	enum class log_level { trace, debug, info, warn, error };

	// Initialize the logging system. Call once at startup.
	void init_logger();

	// Shutdown the logging system. Call once at exit.
	void shutdown_logger();

	// Log a pre-formatted message. spdlog is hidden in the .cpp.
	void log_message(std::string_view category, log_level level, std::string_view message);

	// Set global log level at runtime.
	void set_log_level(log_level level);

	// Set log level for a specific category. Overrides the global level.
	void set_category_level(std::string_view category, log_level level);

	// Set the global log file path. Replaces the default "logs/engine.log".
	// Call before init_logger() or at any time — existing loggers are updated.
	void set_log_file(std::string_view path);

	// Add a dedicated file sink for a specific category.
	// Messages from this category will go to both the global sinks and this file.
	void set_category_file(std::string_view category, std::string_view path);

	// External log callback — editor console or other sinks.
	// Signature: (category, level, message).
	using log_callback_t = std::function<void(std::string_view, log_level, std::string_view)>;
	void set_log_callback(log_callback_t callback);

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
