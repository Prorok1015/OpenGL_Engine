#include <snk/desc/desc_migrations.h>
#include "desc_schema_migrations.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace json = boost::json;

namespace {
	// Stricter than desc::json_pointer_tokens: every part names an object key. An empty part, an escape other than ~0/~1,
	// a header key (__type, __version, __guid) at any depth and an array index are refused, so a step never re-versions
	// a nested object, never turns an array into an object and never shifts elements.
	std::vector<std::string> field_tokens(std::string_view pointer)
	{
		if (pointer.empty() || pointer.front() != '/') {
			throw std::invalid_argument(std::format("'{}' is not a JSON pointer to a field", pointer));
		}
		std::vector<std::string> tokens;
		std::size_t start = 1;
		while (true) {
			const std::size_t slash = pointer.find('/', start);
			const std::string_view raw = pointer.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
			std::string token;
			token.reserve(raw.size());
			for (std::size_t i = 0; i < raw.size(); ++i) {
				if (raw[i] != '~') {
					token.push_back(raw[i]);
				} else if (i + 1 < raw.size() && (raw[i + 1] == '0' || raw[i + 1] == '1')) {
					token.push_back(raw[i + 1] == '0' ? '~' : '/');
					++i;
				} else {
					throw std::invalid_argument(std::format("'{}' has a '~' that is not ~0 or ~1", pointer));
				}
			}
			if (token.empty()) {
				throw std::invalid_argument(std::format("'{}' has an empty part", pointer));
			}
			if (token.starts_with("__")) {
				throw std::invalid_argument(std::format("'{}' points at a header key '{}', not a field", pointer, token));
			}
			if (token == "-" || std::ranges::all_of(token, [](char c) { return c >= '0' && c <= '9'; })) {
				throw std::invalid_argument(std::format("'{}' has an array index '{}': steps do not move array elements", pointer, token));
			}
			tokens.push_back(std::move(token));
			if (slash == std::string_view::npos) {
				return tokens;
			}
			start = slash + 1;
		}
	}

	// The object that holds the last token, through objects only. Without `create`, nullptr when something on the way is
	// missing or a scalar (nothing is there); with it, missing objects are made. An array on the way always throws.
	json::object* holder_of(json::object& object, const std::vector<std::string>& tokens, std::string_view pointer, bool create)
	{
		json::object* node = &object;
		for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
			json::value* next = node->if_contains(tokens[i]);
			if (!next) {
				if (!create) {
					return nullptr;
				}
				next = &node->emplace(tokens[i], json::object{}).first->value();
			}
			if (next->is_array()) {
				throw std::runtime_error(std::format("{} runs through an array, which a step does not enter", pointer));
			}
			if (!next->is_object()) {
				if (!create) {
					return nullptr;
				}
				throw std::runtime_error(std::format("{} runs through a value that is not an object", pointer));
			}
			node = &next->as_object();
		}
		return node;
	}

	// Throws what holder_of(create) would, before anything is changed.
	void check_writable(const json::object& object, const std::vector<std::string>& tokens, std::string_view pointer)
	{
		const json::object* node = &object;
		for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
			const json::value* next = node->if_contains(tokens[i]);
			if (!next) {
				return;
			}
			if (next->is_array()) {
				throw std::runtime_error(std::format("{} runs through an array, which a step does not enter", pointer));
			}
			if (!next->is_object()) {
				throw std::runtime_error(std::format("{} runs through a value that is not an object", pointer));
			}
			node = &next->as_object();
		}
	}

	bool lies_inside(const std::vector<std::string>& inner, const std::vector<std::string>& outer)
	{
		return outer.size() < inner.size() && std::equal(outer.begin(), outer.end(), inner.begin());
	}

	std::string pointer_of_tokens(std::span<const std::string> tokens)
	{
		std::string pointer;
		for (const std::string& token : tokens) {
			pointer += '/';
			for (const char c : token) {
				if (c == '~') pointer += "~0";
				else if (c == '/') pointer += "~1";
				else pointer += c;
			}
		}
		return pointer;
	}

	// rename onto an object that holds the source: the value takes the object's place, so the object may hold nothing
	// but the way down to the source, which would otherwise be lost with it.
	void lift(json::object& object, const std::vector<std::string>& source, const std::vector<std::string>& target,
		std::string_view from, std::string_view to)
	{
		json::value& top = holder_of(object, target, to, false)->at(target.back());
		const json::value* node = &top;
		for (std::size_t i = target.size(); i < source.size(); ++i) {
			const json::object& level = node->as_object();
			for (const auto& member : level) {
				if (member.key() != source[i]) {
					std::vector<std::string> dropped(source.begin(), source.begin() + static_cast<std::ptrdiff_t>(i));
					dropped.emplace_back(member.key());
					throw std::runtime_error(std::format("rename {} -> {} would drop {}", from, to, pointer_of_tokens(dropped)));
				}
			}
			node = &level.at(source[i]);
		}
		json::value value = *node;
		top = std::move(value);
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
			field_tokens(pointer);
		} catch (const std::invalid_argument& error) {
			fail(std::format("{}: {}", where, error.what()));
		}
		return pointer;
	}
}

void desc::migrations::rename(json::object& object, std::string_view from, std::string_view to)
{
	const std::vector<std::string> source = field_tokens(from);
	const std::vector<std::string> target = field_tokens(to);
	const json::object* source_holder = holder_of(object, source, from, false);
	if (!source_holder || !source_holder->contains(source.back())) {
		return;
	}
	if (lies_inside(source, target)) {
		lift(object, source, target, from, to);
		return;
	}
	// Onto a place inside the source: the whole value moves down, whatever it is (a scalar, an array). The way to the
	// target runs through the source itself, which is no obstacle here.
	if (lies_inside(target, source)) {
		json::value wrapped = source_holder->at(source.back());
		for (std::size_t i = target.size(); i-- > source.size();) {
			json::object level;
			level.emplace(target[i], std::move(wrapped));
			wrapped = std::move(level);
		}
		holder_of(object, source, from, false)->at(source.back()) = std::move(wrapped);
		return;
	}
	if (const json::object* target_holder = holder_of(object, target, to, false); target_holder && target_holder->contains(target.back())) {
		throw std::runtime_error(std::format("rename {} -> {}: a value is already at {}", from, to, to));
	}
	check_writable(object, target, to);
	// Written before the source goes, so a failure leaves the object as it was.
	json::value value = source_holder->at(source.back());
	holder_of(object, target, to, true)->insert_or_assign(target.back(), std::move(value));
	// The insert may have moved the objects the source was found in.
	holder_of(object, source, from, false)->erase(source.back());
}

void desc::migrations::remove(json::object& object, std::string_view pointer)
{
	const std::vector<std::string> tokens = field_tokens(pointer);
	if (json::object* holder = holder_of(object, tokens, pointer, false)) {
		holder->erase(tokens.back());
	}
}

void desc::migrations::set_default(json::object& object, std::string_view pointer, json::value value)
{
	const std::vector<std::string> tokens = field_tokens(pointer);
	if (const json::object* holder = holder_of(object, tokens, pointer, false); holder && holder->contains(tokens.back())) {
		return;
	}
	holder_of(object, tokens, pointer, true)->insert_or_assign(tokens.back(), std::move(value));
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
		if (from->as_int64() != static_cast<std::int64_t>(steps.size()) + 1) {
			fail(std::format("{}: 'from' is {}, the steps run from 1 without a gap so it must be {}", where,
				from->as_int64(), steps.size() + 1));
		}
		step.from = static_cast<int>(from->as_int64());
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
					step.renames.emplace_back(std::move(source), std::move(target));
				}
			} else if (key == "remove") {
				if (!member.value().is_array()) {
					fail(std::format("{}: 'remove' must be an array of pointers", where));
				}
				for (const json::value& pointer : member.value().as_array()) {
					std::string removed = pointer_of(pointer, where);
					if (std::ranges::find(step.removals, removed) != step.removals.end()) {
						fail(std::format("{}: remove {} twice", where, removed));
					}
					step.removals.push_back(std::move(removed));
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
		// Renames of one step run one after another: a chain or two onto one key would depend on their order.
		std::unordered_set<std::string> sources;
		std::unordered_set<std::string> targets;
		for (const auto& [source, target] : step.renames) {
			sources.insert(source);
			if (!targets.insert(target).second) {
				fail(std::format("{}: two renames onto {}", where, target));
			}
		}
		for (const auto& [source, target] : step.renames) {
			if (sources.contains(target)) {
				fail(std::format("{}: rename onto {}, which another rename of the step moves away; put them in two steps", where, target));
			}
		}
		// A pointer of the step inside another one (/a and /a/b) makes the outcome depend on which runs first. The two
		// pointers of one rename are no such pair: it lifts its value onto the object that holds it, or moves it down
		// inside itself.
		struct step_pointer
		{
			std::string_view text;
			std::vector<std::string> tokens;
			std::optional<std::size_t> rename;
		};
		std::vector<step_pointer> pointers;
		const auto add_pointer = [&pointers](std::string_view pointer, std::optional<std::size_t> rename) {
			pointers.push_back(step_pointer{ pointer, field_tokens(pointer), rename });
		};
		for (std::size_t i = 0; i < step.renames.size(); ++i) {
			add_pointer(step.renames[i].first, i);
			add_pointer(step.renames[i].second, i);
		}
		for (const auto& pointer : step.removals) {
			add_pointer(pointer, std::nullopt);
		}
		for (const auto& entry : step.defaults) {
			add_pointer(entry.first, std::nullopt);
		}
		for (const auto& outer_pointer : pointers) {
			for (const auto& inner_pointer : pointers) {
				const std::string_view outer = outer_pointer.text;
				const std::string_view inner = inner_pointer.text;
				if (outer_pointer.rename && outer_pointer.rename == inner_pointer.rename) {
					continue;
				}
				if (lies_inside(inner_pointer.tokens, outer_pointer.tokens)) {
					fail(std::format("{}: {} lies inside {}, which the same step also moves; put them in two steps", where, inner, outer));
				}
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
