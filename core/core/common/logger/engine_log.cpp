#include "engine_log.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <mutex>
#include <unordered_map>
#include <string>

namespace {

	constexpr std::size_t MAX_FILE_SIZE  = 5 * 1024 * 1024; // 5 MB
	constexpr std::size_t MAX_FILE_COUNT = 3;
	constexpr const char* FILE_PATTERN   = "[%Y-%m-%d %H:%M:%S.%e] [%n] [%l] %v";

	std::mutex s_logger_mutex;
	std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> s_loggers;
	std::vector<spdlog::sink_ptr> s_sinks;
	spdlog::sink_ptr s_file_sink;
	bool s_initialized = false;
	engine::log_callback_t s_callback;
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
		if (!s_initialized) {
			engine::init_logger();
		}

		std::string key(category);

		{
			std::lock_guard lock(s_logger_mutex);
			auto it = s_loggers.find(key);
			if (it != s_loggers.end()) {
				return it->second;
			}
		}

		auto logger = std::make_shared<spdlog::logger>(key, s_sinks.begin(), s_sinks.end());
		logger->set_level(spdlog::get_level());
		logger->flush_on(spdlog::level::warn);
		spdlog::register_logger(logger);

		{
			std::lock_guard lock(s_logger_mutex);
			s_loggers[key] = logger;
		}

		return logger;
	}

} // anonymous namespace

void engine::init_logger()
{
	if (s_initialized) return;

	// Console sink — colored output
	auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
	console_sink->set_pattern("[%H:%M:%S.%e] [%n] [%^%l%$] %v");
	s_sinks.push_back(console_sink);

	// File sink — rotating
	s_file_sink = make_file_sink(s_log_file_path);
	if (s_file_sink) {
		s_sinks.push_back(s_file_sink);
	}

	// Default level
#ifdef _DEBUG
	spdlog::set_level(spdlog::level::trace);
#else
	spdlog::set_level(spdlog::level::info);
#endif

	s_initialized = true;
}

void engine::shutdown_logger()
{
	std::lock_guard lock(s_logger_mutex);
	s_loggers.clear();
	s_sinks.clear();
	s_file_sink = nullptr;
	s_callback = nullptr;
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

	if (s_callback) {
		s_callback(category, level, message);
	}
}

void engine::set_log_level(log_level level)
{
	auto spd_level = to_spdlog_level(level);
	spdlog::set_level(spd_level);
	std::lock_guard lock(s_logger_mutex);
	for (auto& [name, logger] : s_loggers) {
		logger->set_level(spd_level);
	}
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
	s_callback = std::move(callback);
}
