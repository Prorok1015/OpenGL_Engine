#include <snk/desc/desc_migrations.h>
#include "desc_schema_migrations.h"

#include <cstdint>
#include <format>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#include <snk/desc/desc_json_ops.h>

namespace json = boost::json;

namespace {
	// The header keys say what the object is; a step moves fields, never them.
	void check_field_pointer(std::string_view pointer)
	{
		std::vector<std::string> tokens;
		if (!desc::json_pointer_tokens(pointer, tokens) || tokens.empty()) {
			throw std::invalid_argument(std::format("'{}' is not a JSON pointer to a field", pointer));
		}
		if (tokens.front().starts_with("__")) {
			throw std::invalid_argument(std::format("'{}' points at a header key, not a field", pointer));
		}
	}

	// The pointer helpers work on a json::value; the object is moved in and back, whatever the helper does.
	template<typename Fn>
	void on_root(json::object& object, Fn&& fn)
	{
		json::value root(std::move(object));
		struct restore
		{
			json::value& root;
			json::object& object;
			~restore() { object = std::move(root.as_object()); }
		} guard{ root, object };
		fn(root);
	}

	[[noreturn]] void fail(std::string_view what)
	{
		throw std::runtime_error(std::string{ what });
	}

	std::string pointer_of(const json::value& value, std::string_view where)
	{
		if (!value.is_string()) {
			fail(std::format("{}: a pointer must be a string", where));
		}
		std::string pointer{ value.as_string() };
		try {
			check_field_pointer(pointer);
		} catch (const std::invalid_argument& error) {
			fail(std::format("{}: {}", where, error.what()));
		}
		return pointer;
	}
}

void desc::migrations::rename(json::object& object, std::string_view from, std::string_view to)
{
	check_field_pointer(from);
	check_field_pointer(to);
	on_root(object, [&](json::value& root) {
		const json::value* moved = json_pointer_get(root, from);
		if (!moved) {
			return;
		}
		if (json_pointer_get(root, to)) {
			throw std::runtime_error(std::format("rename {} -> {}: a value is already at {}", from, to, to));
		}
		json::value value = *moved;
		json_pointer_remove(root, from);
		if (!json_pointer_set(root, to, std::move(value))) {
			throw std::runtime_error(std::format("rename {} -> {}: {} runs through a value that is not an object", from, to, to));
		}
	});
}

void desc::migrations::remove(json::object& object, std::string_view pointer)
{
	check_field_pointer(pointer);
	on_root(object, [&](json::value& root) { json_pointer_remove(root, pointer); });
}

void desc::migrations::set_default(json::object& object, std::string_view pointer, json::value value)
{
	check_field_pointer(pointer);
	on_root(object, [&](json::value& root) {
		if (json_pointer_get(root, pointer)) {
			return;
		}
		if (!json_pointer_set(root, pointer, std::move(value))) {
			throw std::runtime_error(std::format("default {}: it runs through a value that is not an object", pointer));
		}
	});
}

std::vector<desc::detail::schema_migration> desc::detail::parse_schema_migrations(const json::array& migrations, int version)
{
	std::vector<schema_migration> steps;
	steps.reserve(migrations.size());
	for (const json::value& raw : migrations) {
		const std::string where = std::format("migration #{}", steps.size() + 1);
		if (!raw.is_object()) {
			fail(std::format("{}: must be an object", where));
		}
		const json::object& object = raw.as_object();
		schema_migration step;
		const json::value* from = object.if_contains("from");
		if (!from || !from->is_int64()) {
			fail(std::format("{}: 'from' must be an integer", where));
		}
		step.from = static_cast<int>(from->as_int64());
		if (from->as_int64() != static_cast<std::int64_t>(steps.size()) + 1) {
			fail(std::format("{}: 'from' is {}, the steps run from 1 without a gap so it must be {}", where,
				from->as_int64(), steps.size() + 1));
		}
		std::unordered_set<std::string> targets;
		for (const auto& member : object) {
			const std::string_view key = member.key();
			if (key == "from") {
				continue;
			}
			if (key == "rename") {
				if (!member.value().is_object()) {
					fail(std::format("{}: 'rename' must be an object of pointer -> pointer", where));
				}
				for (const auto& move : member.value().as_object()) {
					std::string source = pointer_of(json::value(move.key()), where);
					std::string target = pointer_of(move.value(), where);
					if (source == target) {
						fail(std::format("{}: rename {} onto itself", where, source));
					}
					if (!targets.insert(target).second) {
						fail(std::format("{}: two renames onto {}", where, target));
					}
					step.renames.emplace_back(std::move(source), std::move(target));
				}
			} else if (key == "remove") {
				if (!member.value().is_array()) {
					fail(std::format("{}: 'remove' must be an array of pointers", where));
				}
				for (const json::value& pointer : member.value().as_array()) {
					step.removals.push_back(pointer_of(pointer, where));
				}
			} else if (key == "set_default") {
				if (!member.value().is_object()) {
					fail(std::format("{}: 'set_default' must be an object of pointer -> value", where));
				}
				for (const auto& entry : member.value().as_object()) {
					step.defaults.emplace_back(pointer_of(json::value(entry.key()), where), entry.value());
				}
			} else {
				fail(std::format("{}: unknown key '{}'", where, key));
			}
		}
		if (step.renames.empty() && step.removals.empty() && step.defaults.empty()) {
			fail(std::format("{}: no rename, remove or set_default", where));
		}
		steps.push_back(std::move(step));
	}
	if (version != static_cast<int>(steps.size()) + 1) {
		fail(std::format("the schema is version {}, but its {} migration step(s) lead to version {}", version, steps.size(),
			steps.size() + 1));
	}
	return steps;
}

std::string desc::detail::summary_of(const schema_migration& step)
{
	std::string summary;
	const auto add = [&summary](std::string_view part) {
		if (!summary.empty()) {
			summary += "; ";
		}
		summary += part;
	};
	for (const auto& [source, target] : step.renames) {
		add(std::format("rename {} -> {}", source, target));
	}
	for (const auto& pointer : step.removals) {
		add(std::format("remove {}", pointer));
	}
	for (const auto& entry : step.defaults) {
		add(std::format("default {}", entry.first));
	}
	return summary;
}

void desc::detail::apply(const schema_migration& step, json::object& object)
{
	for (const auto& [source, target] : step.renames) {
		migrations::rename(object, source, target);
	}
	for (const auto& pointer : step.removals) {
		migrations::remove(object, pointer);
	}
	for (const auto& [pointer, value] : step.defaults) {
		migrations::set_default(object, pointer, value);
	}
}

std::string desc::schema_migrations_problem(const json::array& migrations, int version)
{
	try {
		detail::parse_schema_migrations(migrations, version);
	} catch (const std::runtime_error& error) {
		return error.what();
	}
	return {};
}
