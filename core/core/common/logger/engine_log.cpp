#include <snk/common/logger/engine_log.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>

namespace {

	constexpr std::size_t MAX_FILE_SIZE  = 5 * 1024 * 1024; // 5 MB
	constexpr std::size_t MAX_FILE_COUNT = 3;
	constexpr const char* FILE_PATTERN   = "[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v";

	std::mutex s_logger_mutex;
	std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> s_loggers;
	std::vector<spdlog::sink_ptr> s_sinks;
	spdlog::sink_ptr s_file_sink;
	bool s_initialized = false;
	// Own copy: spdlog::get_level() dereferences the default logger, which spdlog::shutdown() destroys.
	spdlog::level::level_enum s_global_level = spdlog::level::info;
	// Logging happens on resource/io worker threads while the editor installs its console sink on the
	// main thread; an unguarded std::function swap during a call crashes. Readers copy the pointer
	// under the lock and invoke outside it, so a callback that logs cannot deadlock.
	std::mutex s_callback_mutex;
	std::shared_ptr<const engine::log_callback_t> s_callback;
	std::string s_log_file_path = "logs/engine.log";

	spdlog::level::level_enum to_spdlog_level(engine::log_level level)
	{
		switch (level) {
		case engine::log_level::trace: return spdlog::level::trace;
		case engine::log_level::debug: return spdlog::level::debug;
		case engine::log_level::info:  return spdlog::level::info;
		case engine::log_level::warn:  return spdlog::level::warn;
		case engine::log_level::error: return spdlog::level::err;
		}
		return spdlog::level::info;
	}

	spdlog::sink_ptr make_file_sink(std::string_view path)
	{
		try {
			auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
				std::string(path), MAX_FILE_SIZE, MAX_FILE_COUNT);
			sink->set_pattern(FILE_PATTERN);
			return sink;
		} catch (const spdlog::spdlog_ex&) {
			return nullptr;
		}
	}

	std::shared_ptr<spdlog::logger> get_or_create_logger(std::string_view category)
	{
		std::string key(category);

		// Find-or-create under one lock: two threads logging a new category at once must not both
		// register it (spdlog::register_logger throws on a duplicate name).
		std::lock_guard lock(s_logger_mutex);
		if (!s_initialized) {
			engine::init_logger();
		}
		if (auto it = s_loggers.find(key); it != s_loggers.end()) {
			return it->second;
		}

		auto logger = std::make_shared<spdlog::logger>(key, s_sinks.begin(), s_sinks.end());
		logger->set_level(s_global_level);
		logger->flush_on(spdlog::level::warn);
		spdlog::register_logger(logger);
		s_loggers[key] = logger;
		return logger;
	}

} // anonymous namespace

void engine::init_logger()
{
	if (s_initialized) return;

	// Console sink — colored output
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	console_sink->set_pattern("[%H:%M:%S.%e] [%^%l%$] [%n] %v");
	s_sinks.push_back(console_sink);

	// File sink — rotating
	s_file_sink = make_file_sink(s_log_file_path);
	if (s_file_sink) {
		s_sinks.push_back(s_file_sink);
	}

	// Default level
#ifdef _DEBUG
	s_global_level = spdlog::level::trace;
#else
	s_global_level = spdlog::level::info;
#endif
	spdlog::set_level(s_global_level);

	s_initialized = true;
}

void engine::shutdown_logger()
{
	std::lock_guard lock(s_logger_mutex);
	s_loggers.clear();
	s_sinks.clear();
	s_file_sink = nullptr;
	{
		std::lock_guard cb_lock(s_callback_mutex);
		s_callback.reset();
	}
	spdlog::shutdown();
	s_initialized = false;
}

void engine::log_message(std::string_view category, log_level level, std::string_view message)
{
	auto logger = get_or_create_logger(category);
	auto spd_level = to_spdlog_level(level);

	if (logger->should_log(spd_level)) {
		logger->log(spd_level, "{}", message);
	}

	std::shared_ptr<const engine::log_callback_t> callback;
	{
		std::lock_guard cb_lock(s_callback_mutex);
		callback = s_callback;
	}
	if (callback && *callback) {
		(*callback)(category, level, message);
	}
}

void engine::set_log_level(log_level level)
{
	auto spd_level = to_spdlog_level(level);
	s_global_level = spd_level;
	spdlog::set_level(spd_level);
}

void engine::set_category_level(std::string_view category, log_level level)
{
	auto logger = get_or_create_logger(category);
	logger->set_level(to_spdlog_level(level));
}

void engine::set_log_file(std::string_view path)
{
	s_log_file_path = std::string(path);

	if (!s_initialized) return; // will be applied in init_logger()

	auto new_sink = make_file_sink(path);
	if (!new_sink) return;

	std::lock_guard lock(s_logger_mutex);

	// Replace the old file sink in the global sinks list
	if (s_file_sink) {
		auto it = std::find(s_sinks.begin(), s_sinks.end(), s_file_sink);
		if (it != s_sinks.end()) {
			*it = new_sink;
		}
	} else {
		s_sinks.push_back(new_sink);
	}
	s_file_sink = new_sink;

	// Rebuild all existing loggers with updated sinks
	for (auto& [name, logger] : s_loggers) {
		auto lvl = logger->level();
		spdlog::drop(name);
		auto new_logger = std::make_shared<spdlog::logger>(name, s_sinks.begin(), s_sinks.end());
		new_logger->set_level(lvl);
		new_logger->flush_on(spdlog::level::warn);
		spdlog::register_logger(new_logger);
		logger = new_logger;
	}
}

void engine::set_category_file(std::string_view category, std::string_view path)
{
	auto cat_sink = make_file_sink(path);
	if (!cat_sink) return;

	auto logger = get_or_create_logger(category);

	// Add the category-specific file sink to this logger
	logger->sinks().push_back(cat_sink);
}

void engine::set_log_callback(log_callback_t callback)
{
	auto holder = callback ? std::make_shared<const log_callback_t>(std::move(callback)) : nullptr;
	std::lock_guard cb_lock(s_callback_mutex);
	s_callback = std::move(holder);
}
