#include <snk/common/logger/engine_log_sinks.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <algorithm>
#include <atomic>
#include <deque>
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

	// Listeners (engine::add_log_sink). Logging happens on resource/io worker threads while the editor adds
	// its console sink on the main thread, so the list is copy-on-write: a logging thread takes the pointer
	// under the lock and calls the listeners outside it, so a listener that logs cannot deadlock.
	struct listener
	{
		std::uint64_t id = 0;
		engine::log_callback_t fn;
		// Held while `fn` runs; remove_log_sink takes it, so once removal returns `fn` is not running. Recursive:
		// a listener may log (into itself and the others) or remove its own handle.
		std::recursive_mutex call_mutex;
		std::atomic<bool> removed{ false };
	};
	using listener_list = std::vector<std::shared_ptr<listener>>;
	std::mutex s_listeners_mutex;
	std::shared_ptr<const listener_list> s_listeners;
	std::uint64_t s_next_listener_id = 1;
	// Listeners running on this thread: a message logged from inside one skips the listeners.
	thread_local int t_listener_depth = 0;
	thread_local std::uint64_t t_running_listener_id = 0;
	struct listener_scope
	{
		listener_scope() { ++t_listener_depth; }
		~listener_scope() { --t_listener_depth; }
	};

	// The journal: a ring of the newest messages, seq contiguous from front to back.
	std::mutex s_journal_mutex;
	std::deque<engine::log_entry> s_journal;
	std::size_t s_journal_capacity = engine::default_log_journal_capacity;
	std::uint64_t s_journal_next_seq = 1;
	// -1: follow the logger's own level; otherwise the lowest engine::log_level the journal keeps.
	std::atomic<int> s_journal_min_level{ -1 };
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

	void record_in_journal(std::string_view category, engine::log_level level, std::string_view message)
	{
		// Built before the lock: only the seq needs it.
		engine::log_entry entry;
		entry.time = std::chrono::system_clock::now();
		entry.level = level;
		entry.category.assign(category);
		entry.text.assign(message);

		std::lock_guard lock(s_journal_mutex);
		entry.seq = s_journal_next_seq++;
		if (s_journal_capacity == 0) return;
		while (s_journal.size() >= s_journal_capacity) {
			s_journal.pop_front();
		}
		s_journal.push_back(std::move(entry));
	}

	bool category_matches(const std::string& wanted, const std::string& category)
	{
		if (wanted.empty() || category == wanted) return true;
		return category.size() > wanted.size() && category.compare(0, wanted.size(), wanted) == 0 && category[wanted.size()] == '/';
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
	// Before the logger lock: a listener in flight on another thread may be logging, which needs that lock.
	// The same rule as remove_log_sink: once this returns, no listener is running or will run.
	{
		std::shared_ptr<const listener_list> dropped;
		{
			std::lock_guard listeners_lock(s_listeners_mutex);
			dropped = std::move(s_listeners);
			s_listeners.reset();
		}
		if (dropped) {
			for (const auto& item : *dropped) {
				std::lock_guard call_lock(item->call_mutex);
				item->removed.store(true);
			}
		}
	}
	std::lock_guard lock(s_logger_mutex);
	s_loggers.clear();
	s_sinks.clear();
	s_file_sink = nullptr;
	{
		// The sequence continues: a reader holding a seq from before must not see numbers repeat.
		std::lock_guard journal_lock(s_journal_mutex);
		s_journal.clear();
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

	const int journal_floor = s_journal_min_level.load(std::memory_order_relaxed);
	if (journal_floor < 0 ? logger->should_log(spd_level) : static_cast<int>(level) >= journal_floor) {
		record_in_journal(category, level, message);
	}

	if (t_listener_depth > 0) return;
	std::shared_ptr<const listener_list> listeners;
	{
		std::lock_guard listeners_lock(s_listeners_mutex);
		listeners = s_listeners;
	}
	if (!listeners) return;
	listener_scope scope;
	for (const auto& item : *listeners) {
		std::lock_guard call_lock(item->call_mutex);
		if (item->removed.load()) continue;
		t_running_listener_id = item->id;
		item->fn(category, level, message);
		t_running_listener_id = 0;
	}
}

engine::log_level engine::current_log_level()
{
	switch (s_global_level) {
	case spdlog::level::trace: return log_level::trace;
	case spdlog::level::debug: return log_level::debug;
	case spdlog::level::info: return log_level::info;
	case spdlog::level::warn: return log_level::warn;
	default: return log_level::error;
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

engine::log_sink_handle engine::add_log_sink(log_callback_t callback)
{
	if (!callback) return {};
	auto added = std::make_shared<listener>();
	added->fn = std::move(callback);

	std::lock_guard lock(s_listeners_mutex);
	added->id = s_next_listener_id++;
	auto next = s_listeners ? std::make_shared<listener_list>(*s_listeners) : std::make_shared<listener_list>();
	next->push_back(added);
	s_listeners = std::move(next);
	return log_sink_handle(added->id);
}

std::size_t engine::log_sink_count()
{
	std::lock_guard lock(s_listeners_mutex);
	return s_listeners ? s_listeners->size() : 0;
}

void engine::remove_log_sink(std::uint64_t id)
{
	std::shared_ptr<listener> taken;
	{
		std::lock_guard lock(s_listeners_mutex);
		if (!s_listeners) return;
		const auto found = std::find_if(s_listeners->begin(), s_listeners->end(),
			[id](const std::shared_ptr<listener>& item) { return item->id == id; });
		if (found == s_listeners->end()) return;
		taken = *found;
		auto next = std::make_shared<listener_list>(*s_listeners);
		next->erase(next->begin() + (found - s_listeners->begin()));
		s_listeners = std::move(next);
	}
	// From inside another sink's callback, waiting for this sink's call could lock-cycle with it: take effect without
	// waiting (the flag keeps the ones that already hold a copy of the list away).
	if (t_listener_depth > 0 && id != t_running_listener_id) {
		taken->removed.store(true);
		return;
	}
	// Waits for a call in flight on another thread.
	std::lock_guard call_lock(taken->call_mutex);
	taken->removed.store(true);
}

void engine::set_log_journal_min_level(std::optional<log_level> level)
{
	s_journal_min_level.store(level ? static_cast<int>(*level) : -1, std::memory_order_relaxed);
}

std::optional<engine::log_level> engine::log_journal_min_level()
{
	const int floor = s_journal_min_level.load(std::memory_order_relaxed);
	if (floor < 0) return std::nullopt;
	return static_cast<log_level>(floor);
}

void engine::set_log_journal_capacity(std::size_t entries)
{
	std::lock_guard lock(s_journal_mutex);
	s_journal_capacity = entries;
	while (s_journal.size() > entries) {
		s_journal.pop_front();
	}
}

std::size_t engine::log_journal_capacity()
{
	std::lock_guard lock(s_journal_mutex);
	return s_journal_capacity;
}

std::uint64_t engine::log_journal_next_seq()
{
	std::lock_guard lock(s_journal_mutex);
	return s_journal_next_seq;
}

engine::log_page engine::read_log_journal(const log_query& query)
{
	log_page page;
	const std::size_t limit = std::max<std::size_t>(query.limit, 1);

	std::lock_guard lock(s_journal_mutex);
	const std::uint64_t oldest = s_journal.empty() ? s_journal_next_seq : s_journal.front().seq;
	page.dropped = query.since_seq != 0 && query.since_seq < oldest;
	page.next_seq = s_journal_next_seq;

	std::size_t index = static_cast<std::size_t>(std::max(query.since_seq, oldest) - oldest);
	for (; index < s_journal.size(); ++index) {
		const log_entry& entry = s_journal[index];
		if (entry.level < query.min_level || !category_matches(query.category, entry.category)) continue;
		if (page.entries.size() == limit) {
			page.next_seq = entry.seq;
			break;
		}
		page.entries.push_back(entry);
	}
	return page;
}
