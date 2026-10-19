#include "edt_blackboard_view.h"
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <map>

namespace {
	struct shown
	{
		std::string label;
		std::string text;
	};

	template <class T, class Format>
	bool describe_as(const ecs::bb_entry& entry, std::string_view label, Format format, shown& out)
	{
		if (entry.type != entt::type_hash<T>::value()) return false;
		out.label = std::string{ label };
		out.text = format(*static_cast<const T*>(entry.value.get()));
		return true;
	}

	shown describe(const ecs::bb_entry& entry)
	{
		shown out;
		if (!entry.value) {
			out.label = std::string{ entry.type_name };
			out.text = "<empty>";
			return out;
		}
		const auto plain = [](const auto& v) { return std::format("{}", v); };
		if (describe_as<bool>(entry, "bool", [](bool v) { return std::string{ v ? "true" : "false" }; }, out)) return out;
		if (describe_as<int>(entry, "int", plain, out)) return out;
		if (describe_as<unsigned>(entry, "uint", plain, out)) return out;
		if (describe_as<std::int64_t>(entry, "int64", plain, out)) return out;
		if (describe_as<std::uint64_t>(entry, "uint64", plain, out)) return out;
		if (describe_as<float>(entry, "float", plain, out)) return out;
		if (describe_as<double>(entry, "double", plain, out)) return out;
		if (describe_as<std::string>(entry, "string", [](const std::string& v) { return std::format("\"{}\"", v); }, out)) return out;
		if (describe_as<glm::vec2>(entry, "vec2", [](const glm::vec2& v) { return std::format("({}, {})", v.x, v.y); }, out)) return out;
		if (describe_as<glm::vec3>(entry, "vec3", [](const glm::vec3& v) { return std::format("({}, {}, {})", v.x, v.y, v.z); }, out)) return out;
		if (describe_as<glm::vec4>(entry, "vec4", [](const glm::vec4& v) { return std::format("({}, {}, {}, {})", v.x, v.y, v.z, v.w); }, out)) return out;
		out.label = std::string{ entry.type_name };
		out.text = std::format("<{}>", entry.type_name);
		return out;
	}

	bool contains_ignoring_case(std::string_view text, std::string_view part)
	{
		if (part.empty()) return true;
		const auto it = std::search(text.begin(), text.end(), part.begin(), part.end(), [](char a, char b) {
			return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
		});
		return it != text.end();
	}
}

std::string edt::blackboard_view::owner_of(std::string_view key)
{
	const auto dot = key.find('.');
	return dot == std::string_view::npos ? std::string{} : std::string{ key.substr(0, dot) };
}

std::string edt::blackboard_view::type_label(const ecs::bb_entry& entry)
{
	return describe(entry).label;
}

std::string edt::blackboard_view::format_value(const ecs::bb_entry& entry)
{
	return describe(entry).text;
}

std::vector<edt::blackboard_view::row> edt::blackboard_view::collect_rows(const scn::context_set& set, std::string_view filter)
{
	std::map<uint32_t, std::string> names;
	set.for_each([&](const scn::world_context& context) { names.emplace(context.get_id(), std::string{ context.get_name() }); });

	const auto& committed = set.channel().blackboard().committed();
	std::vector<row> rows;
	rows.reserve(committed.size());
	for (const auto& [key, entry] : committed) {
		if (!contains_ignoring_case(key, filter)) continue;
		row r;
		r.key = key;
		r.owner = owner_of(key);
		shown s = describe(entry);
		r.type = std::move(s.label);
		r.value = std::move(s.text);
		if (entry.writer == ecs::blackboard_data::external_writer) {
			r.writer = "external";
		} else {
			const auto it = names.find(static_cast<uint32_t>(entry.writer));
			r.writer = it != names.end() ? it->second : std::format("context {}", entry.writer);
		}
		rows.push_back(std::move(r));
	}
	return rows;
}

std::vector<std::string> edt::blackboard_view::set_names(scn::context_manager& manager)
{
	std::vector<std::string> names;
	manager.for_each_set([&](scn::context_set& set) { names.push_back(set.name()); });
	return names;
}

std::string edt::blackboard_view::pick_set(std::string_view current, const std::vector<std::string>& names)
{
	const auto has = [&](std::string_view name) {
		return std::any_of(names.begin(), names.end(), [&](const std::string& n) { return std::string_view{ n } == name; });
	};
	if (names.empty()) return {};
	if (has(current)) return std::string{ current };
	if (has(scn::context_manager::level_set_name)) return std::string{ scn::context_manager::level_set_name };
	return names.front();
}
