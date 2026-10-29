#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace json = boost::json;

namespace {
	constexpr std::size_t DEFAULT_LIMIT = 100;

	const char* level_name(engine::log_level level)
	{
		switch (level) {
		case engine::log_level::trace: return "trace";
		case engine::log_level::debug: return "debug";
		case engine::log_level::info:  return "info";
		case engine::log_level::warn:  return "warn";
		case engine::log_level::error: return "error";
		}
		return "info";
	}

	std::optional<engine::log_level> parse_level(std::string_view name)
	{
		for (const auto level : { engine::log_level::trace, engine::log_level::debug, engine::log_level::info,
			engine::log_level::warn, engine::log_level::error }) {
			if (name == level_name(level)) return level;
		}
		return std::nullopt;
	}

	// The schema accepts 5, 5.0 and a uint64; a negative one never passes `minimum`.
	std::optional<std::uint64_t> as_count(const json::value& value)
	{
		const auto number = cmd::json_schema::integer_of(value);
		if (!number || *number < 0) return std::nullopt;
		return static_cast<std::uint64_t>(*number);
	}

	cmd::result read_log(const cmd::call& request)
	{
		const json::object& args = request.args.as_object();
		engine::log_query query;
		query.limit = DEFAULT_LIMIT;
		if (const auto* since = args.if_contains("since_seq")) {
			const auto count = as_count(*since);
			if (!count) return cmd::result::failure(cmd::codes::invalid_args, "since_seq is not an integer that fits");
			query.since_seq = *count;
		}
		if (const auto* level = args.if_contains("min_level")) {
			const auto parsed = parse_level(level->as_string());
			if (!parsed) return cmd::result::failure(cmd::codes::invalid_args, "min_level is not a log level");
			query.min_level = *parsed;
		}
		if (const auto* category = args.if_contains("category")) query.category = std::string(category->as_string());
		if (const auto* limit = args.if_contains("limit")) {
			const auto count = as_count(*limit);
			if (!count) return cmd::result::failure(cmd::codes::invalid_args, "limit is not an integer that fits");
			query.limit = static_cast<std::size_t>(*count);
		}

		const engine::log_page page = engine::read_log_journal(query);
		json::array entries;
		entries.reserve(page.entries.size());
		for (const auto& entry : page.entries) {
			const auto since_epoch = std::chrono::duration_cast<std::chrono::milliseconds>(entry.time.time_since_epoch());
			entries.push_back(json::object{
				{ "seq", static_cast<std::int64_t>(entry.seq) },
				{ "time_ms", static_cast<std::int64_t>(since_epoch.count()) },
				{ "level", json::string(level_name(entry.level)) },
				{ "category", json::string(entry.category) },
				{ "text", json::string(entry.text) } });
		}
		return cmd::result::success(json::object{
			{ "entries", std::move(entries) },
			{ "next_seq", static_cast<std::int64_t>(page.next_seq) },
			{ "dropped", page.dropped } });
	}
}

cmd::registration cmd::register_log_commands(command_registry& registry)
{
	command_desc desc;
	desc.id = "log.read";
	desc.summary = "Read the engine log journal: entries since a sequence number, filtered by level and category";
	desc.category = cmd::agent_category::read;
	desc.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"since_seq": { "type": "integer", "minimum": 0 },
			"min_level": { "type": "string", "enum": ["trace", "debug", "info", "warn", "error"] },
			"category": { "type": "string" },
			"limit": { "type": "integer", "minimum": 1, "maximum": 500 }
		},
		"additionalProperties": false
	})");
	desc.result_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"entries": {
				"type": "array",
				"items": {
					"type": "object",
					"properties": {
						"seq": { "type": "integer" },
						"time_ms": { "type": "integer" },
						"level": { "type": "string", "enum": ["trace", "debug", "info", "warn", "error"] },
						"category": { "type": "string" },
						"text": { "type": "string" }
					},
					"required": ["seq", "time_ms", "level", "category", "text"],
					"additionalProperties": false
				}
			},
			"next_seq": { "type": "integer" },
			"dropped": { "type": "boolean" }
		},
		"required": ["entries", "next_seq", "dropped"],
		"additionalProperties": false
	})");
	desc.invoke = read_log;
	return registry.add(std::move(desc));
}
