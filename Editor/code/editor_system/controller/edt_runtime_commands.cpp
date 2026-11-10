#include "edt_runtime_commands.h"
#include "edt_code_commands.h"
#include "edt_play_session.h"
#include "edt_vcs_policy.h"
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_types.h>
#include <snk/common/engine_assert.h>
#include <snk/editor/edt_editor_events.h>
#include "edt_state_commands.h"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/scene/scn_time.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace json = boost::json;

namespace {

	constexpr std::size_t k_max_glob = 128;
	constexpr std::size_t k_max_filter_components = 32;
	constexpr std::size_t k_max_children_listed = 100;
	constexpr std::size_t k_max_subtree = 100000;

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "the schema of a runtime command is not JSON: {}", ec.message());
		return parsed;
	}

	json::string safe_string(std::string_view text)
	{
		if (edt::is_valid_utf8(text)) return json::string(text);
		std::string cleaned(text);
		for (char& c : cleaned) {
			if (static_cast<unsigned char>(c) >= 0x80) c = '?';
		}
		return json::string(cleaned);
	}

	cmd::result failure(const char* code, std::string message)
	{
		return cmd::result::failure(code, std::move(message));
	}

	std::string lower(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return out;
	}

	// Without a wildcard a substring, as scene.query does.
	struct name_filter
	{
		explicit name_filter(std::string text)
			: glob(std::move(text)), lowered(lower(glob)), wildcard(glob.find_first_of("*?") != std::string::npos)
		{
		}

		bool empty() const { return glob.empty(); }
		bool matches(std::string_view name) const
		{
			return wildcard ? edt::vcs_glob_match(glob, name, true) : lower(name).find(lowered) != std::string::npos;
		}

		std::string glob;
		std::string lowered;
		bool wildcard;
	};

	// Strings of a component value go into the answer: bytes that are not UTF-8 would make the whole reply invalid.
	void sanitize(json::value& value)
	{
		if (value.is_string()) {
			if (!edt::is_valid_utf8(std::string_view(value.as_string()))) {
				json::string cleaned = safe_string(std::string_view(value.as_string()));
				value = std::move(cleaned);
			}
		} else if (value.is_object()) {
			for (auto& member : value.as_object()) sanitize(member.value());
		} else if (value.is_array()) {
			for (auto& item : value.as_array()) sanitize(item);
		}
	}

	std::uint64_t id_of(entt::entity entity)
	{
		return static_cast<std::uint64_t>(entt::to_integral(entity));
	}

	// The world every answer is about, with the checks every command shares.
	struct target
	{
		edt::play_session* play = nullptr;
		scn::context_set* set = nullptr;
		scn::world_context* world = nullptr;
		const scn::component_catalog* catalog = nullptr;
	};

	std::optional<target> find_target(const edt::runtime_commands_deps& deps, const json::object& args, cmd::result& error)
	{
		target found;
		found.play = deps.play ? deps.play() : nullptr;
		if (found.play && found.play->reloading()) {
			error = failure(cmd::codes::busy, "a module reload is rebuilding Play's world; call again once it runs");
			return std::nullopt;
		}
		found.set = deps.play_set ? deps.play_set() : nullptr;
		if (!found.play || !found.set || found.play->state() == edt::play_state::stopped) {
			error = failure(cmd::codes::not_found, "Play is not running (these commands show the live state of Play; scene.query / node.get show the document)");
			return std::nullopt;
		}
		const std::vector<std::string> names = found.play->world_names();
		std::string wanted;
		if (const json::value* given = args.if_contains("world")) {
			wanted = std::string(given->as_string());
		} else if (!names.empty()) {
			wanted = names.front();
		}
		found.world = found.set->find(wanted);
		if (!found.world) {
			std::string listed;
			for (const std::string& name : names) listed += (listed.empty() ? "" : ", ") + name;
			error = failure(cmd::codes::not_found, std::format("no world '{}' in Play (the worlds: {})", wanted, listed));
			return std::nullopt;
		}
		found.catalog = found.set->layer() ? &found.set->layer()->catalog() : nullptr;
		if (!found.catalog) {
			error = failure(cmd::codes::not_found, "Play has no component catalog");
			return std::nullopt;
		}
		return found;
	}

	void write_header(json::object& out, const target& at)
	{
		out["source"] = "play";
		out["note"] = "the live state of Play, not the document";
		out["play_state"] = json::string(edt::play_state_name(at.play->state()));
		out["frame"] = static_cast<std::int64_t>(at.play->frames_run());
		out["reload"] = static_cast<std::int64_t>(at.play->reload_number());
		out["world"] = safe_string(at.world->get_name());
		if (const auto* time = at.world->state().ctx().find<scn::world_time>()) out["world_time"] = time->seconds;
	}

	std::string name_of(const entt::registry& reg, entt::entity entity)
	{
		const auto* name = reg.try_get<scn::name_component>(entity);
		return name ? name->name : std::string{};
	}

	bool has_children(const entt::registry& reg, entt::entity entity)
	{
		const auto* children = reg.try_get<scn::children_component>(entity);
		return children && !children->children.empty();
	}

	json::array names_json(const std::vector<std::string>& names)
	{
		json::array out;
		out.reserve(names.size());
		for (const std::string& name : names) out.emplace_back(safe_string(name));
		return out;
	}

	json::object summary_of(const entt::registry& reg, entt::entity entity, const scn::entity_writer& writer)
	{
		json::object item;
		item["entity"] = id_of(entity);
		item["name"] = safe_string(name_of(reg, entity));
		item["components"] = names_json(writer.present(reg, entity));
		item["has_children"] = has_children(reg, entity);
		return item;
	}

	std::uint32_t index_of(entt::entity entity)
	{
		return static_cast<std::uint32_t>(entt::to_entity(entity));
	}

	// The editor's own entities (its cameras) are not part of the game.
	bool shown(const entt::registry& reg, const scn::entity_writer& writer, entt::entity entity)
	{
		return reg.valid(entity) && !writer.editor_only(reg, entity);
	}

	// The entity of an id the caller gave: the whole id with its version, so a reused slot is not the same entity.
	std::optional<entt::entity> entity_of(const entt::registry& reg, const json::value& value)
	{
		const auto number = cmd::json_schema::integer_of(value);
		if (!number || *number < 0 || *number > 0xFFFFFFFFLL) return std::nullopt;
		const auto entity = static_cast<entt::entity>(static_cast<std::uint32_t>(*number));
		if (!reg.valid(entity)) return std::nullopt;
		return entity;
	}

	// False when the walk was cut at k_max_subtree entities.
	bool collect_subtree(const entt::registry& reg, entt::entity root, std::unordered_set<std::uint32_t>& out)
	{
		std::vector<entt::entity> pending{ root };
		while (!pending.empty()) {
			if (out.size() >= k_max_subtree) return false;
			const entt::entity current = pending.back();
			pending.pop_back();
			if (!out.insert(static_cast<std::uint32_t>(entt::to_integral(current))).second) continue;
			if (const auto* children = reg.try_get<scn::children_component>(current)) {
				for (const entt::entity child : children->children) {
					if (reg.valid(child)) pending.push_back(child);
				}
			}
		}
		return true;
	}

	// What a cursor is tied to: the Start, the module reload and the frame it was made in (a reload rebuilds the entities
	// and keeps the frame count), the query it continues (world and filter) and the entity it continues after. A position
	// would skip or repeat entities when one is made or destroyed in between.
	struct cursor
	{
		std::uint64_t run = 0;
		std::uint64_t reload = 0;
		std::uint64_t frame = 0;
		std::uint64_t query = 0;
		std::uint32_t after = 0;
	};

	std::string format_cursor(const cursor& at)
	{
		return std::format("{}.{}.{}.{:x}.{}", at.run, at.reload, at.frame, at.query, at.after);
	}

	std::optional<cursor> parse_cursor(std::string_view text)
	{
		if (text.size() > 80) return std::nullopt;
		cursor out;
		std::string_view rest = text;
		const auto take = [&rest](auto& value, int base) {
			const std::size_t dot = rest.find('.');
			const std::string_view part = rest.substr(0, dot);
			const auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), value, base);
			if (part.empty() || ec != std::errc{} || end != part.data() + part.size()) return false;
			rest = dot == std::string_view::npos ? std::string_view{} : rest.substr(dot + 1);
			return dot != std::string_view::npos || rest.empty();
		};
		if (!take(out.run, 10) || !take(out.reload, 10) || !take(out.frame, 10) || !take(out.query, 16) || rest.empty() || !take(out.after, 10) || !rest.empty()) {
			return std::nullopt;
		}
		return out;
	}

	std::uint64_t hash_of(std::initializer_list<std::string_view> parts, std::uint64_t number)
	{
		std::uint64_t hash = 1469598103934665603ull;
		const auto mix = [&hash](unsigned char byte) { hash = (hash ^ byte) * 1099511628211ull; };
		for (const std::string_view part : parts) {
			for (const char c : part) mix(static_cast<unsigned char>(c));
			mix(0);
		}
		for (int shift = 0; shift < 64; shift += 8) mix(static_cast<unsigned char>(number >> shift));
		return hash;
	}

	cmd::result stale_cursor(std::uint64_t frame)
	{
		cmd::result error = failure(cmd::codes::stale,
			std::format("the cursor is from another frame, another module reload or another Play session (now frame {}); pause the game (play.pause) or start the query again", frame));
		error.value = json::object{ { "frame", static_cast<std::int64_t>(frame) } };
		return error;
	}

	cmd::result runtime_query(const edt::runtime_commands_deps& deps, const cmd::call& call)
	{
		const json::object& args = call.args.as_object();
		cmd::result error;
		const auto at = find_target(deps, args, error);
		if (!at) return error;
		const entt::registry& reg = at->world->state();
		const scn::entity_writer writer(*at->catalog);
		const std::uint64_t frame = at->play->frames_run();
		const std::uint64_t run = at->play->run_number();
		const std::uint64_t reload = at->play->reload_number();

		name_filter names{ std::string{} };
		std::vector<const scn::component_entry*> required;
		std::optional<entt::entity> under;
		if (const json::value* filter = args.if_contains("filter")) {
			const json::object& f = filter->as_object();
			if (const json::value* given = f.if_contains("name_glob")) {
				std::string glob(given->as_string());
				if (glob.size() > k_max_glob) return failure(cmd::codes::invalid_args, std::format("a glob is limited to {} characters", k_max_glob));
				names = name_filter(std::move(glob));
			}
			if (const json::value* given = f.if_contains("has_component")) {
				if (given->as_array().size() > k_max_filter_components) {
					return failure(cmd::codes::invalid_args, std::format("has_component takes at most {} names", k_max_filter_components));
				}
				for (const json::value& name : given->as_array()) {
					const scn::component_entry* entry = at->catalog->find(std::string_view(name.as_string()));
					if (!entry || entry->category != scn::component_category::data) {
						return failure(cmd::codes::invalid_args, std::format("no component '{}' in the catalog", std::string(name.as_string())));
					}
					required.push_back(entry);
				}
			}
			if (const json::value* given = f.if_contains("under")) {
				under = entity_of(reg, *given);
				if (!under) return failure(cmd::codes::not_found, "filter.under is not a live entity of this world");
			}
		}

		std::uint64_t query_hash = hash_of({ at->world->get_name(), names.glob }, under ? id_of(*under) : 0);
		for (const scn::component_entry* entry : required) query_hash = hash_of({ entry->name }, query_hash);

		std::optional<std::uint32_t> after;
		if (const json::value* given = args.if_contains("cursor")) {
			const auto parsed = parse_cursor(std::string_view(given->as_string()));
			if (!parsed) return failure(cmd::codes::invalid_args, "not a cursor of runtime.query");
			if (parsed->run != run || parsed->reload != reload || parsed->frame != frame) return stale_cursor(frame);
			if (parsed->query != query_hash) return failure(cmd::codes::invalid_args, "the cursor belongs to another query (another world or filter)");
			after = parsed->after;
		}
		std::int64_t page_size = edt::state_query_default_page_size;
		if (const json::value* given = args.if_contains("page_size")) {
			if (const auto number = cmd::json_schema::integer_of(*given)) page_size = *number;
		}
		page_size = std::clamp<std::int64_t>(page_size, 1, edt::state_query_max_page_size);

		std::unordered_set<std::uint32_t> subtree;
		const bool subtree_complete = !under || collect_subtree(reg, *under, subtree);

		std::vector<entt::entity> live;
		if (const auto* storage = reg.storage<entt::entity>()) {
			for (const entt::entity entity : *storage) {
				if (after && index_of(entity) <= *after) continue;
				if (shown(reg, writer, entity)) live.push_back(entity);
			}
		}
		std::ranges::sort(live, {}, index_of);

		json::array items;
		std::size_t bytes = 0;
		bool more = false;
		std::uint32_t last = 0;
		for (const entt::entity entity : live) {
			if (under && !subtree.contains(index_of(entity))) continue;
			if (!names.empty() && !names.matches(name_of(reg, entity))) continue;
			if (!std::ranges::all_of(required, [&](const scn::component_entry* entry) { return entry->contains(reg, entity); })) continue;
			if (static_cast<std::int64_t>(items.size()) >= page_size || bytes >= edt::runtime_page_max_bytes) {
				more = true;
				break;
			}
			json::object item = summary_of(reg, entity, writer);
			bytes += json::serialize(item).size();
			items.push_back(std::move(item));
			last = index_of(entity);
		}

		json::object out;
		write_header(out, *at);
		out["items"] = std::move(items);
		if (!subtree_complete) out["subtree_truncated"] = true;
		if (more) out["next_cursor"] = format_cursor({ run, reload, frame, query_hash, last });
		return cmd::result::success(std::move(out));
	}

	cmd::result runtime_get(const edt::runtime_commands_deps& deps, const cmd::call& call)
	{
		const json::object& args = call.args.as_object();
		cmd::result error;
		const auto at = find_target(deps, args, error);
		if (!at) return error;
		const entt::registry& reg = at->world->state();
		const auto entity = entity_of(reg, args.at("entity"));
		const scn::entity_writer writer(*at->catalog);
		if (!entity || !shown(reg, writer, *entity)) {
			return failure(cmd::codes::not_found, "no such entity in this world (it is gone, or its slot was reused by another entity)");
		}

		std::vector<std::string> only;
		const std::vector<std::string>* filter = nullptr;
		if (const json::value* given = args.if_contains("components")) {
			for (const json::value& name : given->as_array()) {
				const scn::component_entry* entry = at->catalog->find(std::string_view(name.as_string()));
				if (!entry || entry->category != scn::component_category::data) {
					return failure(cmd::codes::invalid_args, std::format("no component '{}' in the catalog", std::string(name.as_string())));
				}
				only.emplace_back(name.as_string());
			}
			filter = &only;
		}
		scn::entity_writer::components parts = writer.write(reg, *entity, filter);

		json::object components;
		json::array truncated;
		std::size_t bytes = 0;
		for (auto& member : parts.saved) {
			sanitize(member.value());
			const std::size_t size = json::serialize(member.value()).size() + member.key().size();
			if (bytes + size > edt::runtime_entity_max_bytes) {
				truncated.emplace_back(safe_string(member.key()));
				continue;
			}
			bytes += size;
			components[member.key()] = std::move(member.value());
		}

		json::object out;
		write_header(out, *at);
		out["entity"] = id_of(*entity);
		out["name"] = safe_string(name_of(reg, *entity));
		const auto* parent = reg.try_get<scn::parent_component>(*entity);
		if (parent && shown(reg, writer, parent->parent)) {
			out["parent"] = summary_of(reg, parent->parent, writer);
		} else {
			out["parent"] = nullptr;
		}
		json::array children;
		std::size_t child_count = 0;
		if (const auto* listed = reg.try_get<scn::children_component>(*entity)) {
			for (const entt::entity child : listed->children) {
				if (!shown(reg, writer, child)) continue;
				if (children.size() < k_max_children_listed) children.push_back(summary_of(reg, child, writer));
				++child_count;
			}
		}
		out["children"] = std::move(children);
		out["child_count"] = static_cast<std::int64_t>(child_count);
		out["children_truncated"] = child_count > k_max_children_listed;
		out["components"] = std::move(components);
		out["unsaved"] = names_json(parts.unsaved);
		out["truncated"] = std::move(truncated);
		return cmd::result::success(std::move(out));
	}

	void keep(std::vector<edt::registration>& out, edt::registration added)
	{
		if (added.active()) out.push_back(std::move(added));
	}

	constexpr std::string_view k_header_properties = R"json("source":{"type":"string","enum":["play"]},"note":{"type":"string"},
		"play_state":{"type":"string","enum":["playing","paused"]},"frame":{"type":"integer"},
		"reload":{"type":"integer","description":"module reloads that rebuilt this Play's world; entity ids read before one name other entities"},
		"world":{"type":"string"},"world_time":{"type":"number"})json";

	constexpr std::string_view k_summary = R"({"type":"object","properties":{"entity":{"type":"integer"},"name":{"type":"string"},
		"components":{"type":"array","items":{"type":"string"}},"has_children":{"type":"boolean"}},"required":["entity","name","components","has_children"]})";
}

std::vector<edt::registration> edt::register_runtime_commands(cmd::command_registry& commands, runtime_commands_deps deps)
{
	std::vector<registration> out;
	auto shared = std::make_shared<runtime_commands_deps>(std::move(deps));

	cmd::command_desc query;
	query.id = "runtime.query";
	query.summary = "Page through the live entities of a world of the running game (Play): entity id, name, component names. Filter by name, component or subtree; follow next_cursor (pause the game first for a consistent walk). Read-only; this is the state of Play, not the document";
	query.category = cmd::agent_category::read;
	query.args_schema = parse_schema(std::format(R"json({{"type":"object","properties":{{
		"world":{{"type":"string","description":"world name; default the first world of Play"}},
		"filter":{{"type":"object","properties":{{
			"name_glob":{{"type":"string","description":"* and ? wildcards, case-insensitive; without them a substring"}},
			"has_component":{{"type":"array","items":{{"type":"string"}},"description":"catalog names that must all be present"}},
			"under":{{"type":"integer","minimum":0,"description":"an entity id: only it and what lies under it"}}}},"additionalProperties":false}},
		"page_size":{{"type":"integer","minimum":1,"maximum":{},"description":"entities per page, at most {}, default {}"}},
		"cursor":{{"type":"string","description":"next_cursor of the previous page of the same query; stale once a frame has passed, a module reload rebuilt the world or the game was restarted"}}}},"additionalProperties":false}})json",
		state_query_max_page_size, state_query_max_page_size, state_query_default_page_size));
	query.result_schema = parse_schema(std::format(R"({{"type":"object","properties":{{{},
		"items":{{"type":"array","items":{}}},"next_cursor":{{"type":"string"}},"subtree_truncated":{{"type":"boolean","description":"filter.under reached the limit of entities it walks"}}}},"required":["source","frame","items"]}})", k_header_properties, k_summary));
	query.invoke = [shared](const cmd::call& call) { return runtime_query(*shared, call); };
	keep(out, commands.add(std::move(query)));

	cmd::command_desc get;
	get.id = "runtime.get";
	get.summary = "One live entity of the running game (Play) by its whole entity id: name, parent, children, components as desc JSON. Components without a schema are only named (unsaved); what does not fit 256 KiB is named in truncated. Read-only; the state of Play, not the document. An entity id read before a module reload (see "reload" in the answers) may name another entity now";
	get.category = cmd::agent_category::read;
	get.args_schema = parse_schema(R"({"type":"object","properties":{
		"entity":{"type":"integer","minimum":0,"description":"the entity id from runtime.query, with its version"},
		"world":{"type":"string","description":"world name; default the first world of Play"},
		"components":{"type":"array","items":{"type":"string"},"description":"only these catalog names; default all"}},
		"required":["entity"],"additionalProperties":false})");
	get.result_schema = parse_schema(std::format(R"({{"type":"object","properties":{{{},
		"entity":{{"type":"integer"}},"name":{{"type":"string"}},"parent":{{"type":["object","null"]}},
		"children":{{"type":"array","items":{}}},"child_count":{{"type":"integer"}},"children_truncated":{{"type":"boolean"}},
		"components":{{"type":"object"}},"unsaved":{{"type":"array","items":{{"type":"string"}}}},"truncated":{{"type":"array","items":{{"type":"string"}}}}}},
		"required":["source","frame","entity","name","components"]}})", k_header_properties, k_summary));
	get.invoke = [shared](const cmd::call& call) { return runtime_get(*shared, call); };
	keep(out, commands.add(std::move(get)));
	return out;
}
