#include "edt_bus_commands.h"
#include "edt_code_commands.h"
#include "edt_play_session.h"
#include <snk/command/cmd_types.h>
#include <snk/common/engine_assert.h>
#include <snk/ecs/ecs_bus.hpp>
#include <snk/editor/edt_editor_events.h>
#include <snk/input/inp_action_state.h>
#include <snk/input/inp_input_router.h>
#include <snk/input/inp_input_stack.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_world_context.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {

	constexpr std::string_view k_no_play = "no Play session: world events exist only while Play runs";
	constexpr std::string_view k_play_set = "play";
	constexpr std::size_t k_default_last = 32;
	constexpr std::size_t k_max_type_filter = 256;

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "the schema of a bus command is not JSON: {}", ec.message());
		return parsed;
	}

	// Names come from the game module and the level file.
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

	struct play_target
	{
		edt::play_session* play = nullptr;
		scn::context_set* set = nullptr;
	};

	// Only the set "play": the editor's worlds, the level in Edit and the previews are never shown.
	std::optional<play_target> find_play(const edt::bus_commands_deps& deps, const json::object& args, cmd::result& error)
	{
		if (const json::value* set = args.if_contains("set"); set && set->as_string() != k_play_set) {
			error = failure(cmd::codes::invalid_args, "only the worlds of Play are shown: set must be \"play\" or left out");
			return std::nullopt;
		}
		play_target found;
		found.play = deps.play ? deps.play() : nullptr;
		if (found.play && found.play->reloading()) {
			error = failure(cmd::codes::busy, "a module reload is rebuilding Play's world; call again once it runs");
			return std::nullopt;
		}
		found.set = deps.play_set ? deps.play_set() : nullptr;
		if (!found.play || !found.set || found.play->state() == edt::play_state::stopped) {
			error = failure(cmd::codes::not_found, std::string{ k_no_play });
			return std::nullopt;
		}
		return found;
	}

	scn::world_context* find_world(const play_target& at, const json::object& args, cmd::result& error)
	{
		const std::vector<std::string> names = at.play->world_names();
		std::string wanted;
		if (const json::value* given = args.if_contains("world")) {
			wanted = std::string(given->as_string());
		} else if (!names.empty()) {
			wanted = names.front();
		}
		scn::world_context* world = at.set->find(wanted);
		if (!world) {
			std::string listed;
			for (const std::string& name : names) listed += (listed.empty() ? "" : ", ") + name;
			error = failure(cmd::codes::not_found, std::format("no world '{}' in Play (the worlds: {})", wanted, listed));
		}
		return world;
	}

	void write_header(json::object& out, const play_target& at)
	{
		out["source"] = "play";
		out["play_state"] = json::string(edt::play_state_name(at.play->state()));
		out["frame"] = static_cast<std::int64_t>(at.play->frames_run());
		out["reload"] = static_cast<std::int64_t>(at.play->reload_number());
	}

	// "inp::action_event" is asked for as itself or as "action_event".
	bool type_matches(std::string_view type, std::string_view wanted)
	{
		if (type == wanted) return true;
		return type.size() > wanted.size() + 2 && type.ends_with(wanted) && type.substr(type.size() - wanted.size() - 2, 2) == "::";
	}

	json::value payload_of(const std::string& text)
	{
		if (text.empty()) return nullptr;
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		if (ec) return json::object{ { "error", "the payload writer gave no JSON" } };
		return parsed;
	}

	// The payload of inp::action_event holds the action id only (its writer has no map): the name comes from the map the
	// world reads now. An event older than a change of the map gets no name rather than a wrong one.
	void name_action(json::value& payload, const inp::action_map* map, std::uint64_t map_since_delivery, std::uint64_t delivery)
	{
		if (!map || delivery < map_since_delivery || !payload.is_object()) return;
		json::object& fields = payload.as_object();
		const json::value* id = fields.if_contains("action");
		if (!id || !id->is_number()) return;
		const auto action = id->to_number<std::int64_t>();
		if (action < 0 || static_cast<std::size_t>(action) >= map->size()) return;
		fields["name"] = safe_string(map->actions()[static_cast<std::size_t>(action)].name);
	}

	cmd::result world_events(const edt::bus_commands_deps& deps, const cmd::call& call)
	{
		const json::object& args = call.args.as_object();
		cmd::result error;
		const auto at = find_play(deps, args, error);
		if (!at) return error;
		scn::world_context* world = find_world(*at, args, error);
		if (!world) return error;

		std::string type;
		if (const json::value* given = args.if_contains("type")) type = std::string(given->as_string());
		if (type.size() > k_max_type_filter) return failure(cmd::codes::invalid_args, "type is longer than 256 bytes");
		std::size_t last = k_default_last;
		if (const json::value* given = args.if_contains("last")) last = static_cast<std::size_t>(given->to_number<std::int64_t>());
		last = std::min(last, edt::play_journal_capacity);

		const auto* bus = std::as_const(world->state()).ctx().find<ecs::world_bus>();
		const ecs::bus_journal* journal = bus ? bus->journal() : nullptr;
		if (!journal) {
			return failure(cmd::codes::not_found, std::format("the world '{}' of Play keeps no journal of its events", world->get_name()));
		}

		std::vector<const ecs::bus_journal_entry*> picked;
		picked.reserve(last);
		const auto& entries = journal->entries();
		for (auto it = entries.rbegin(); it != entries.rend() && picked.size() < last; ++it) {
			if (type.empty() || type_matches(it->type, type)) picked.push_back(&*it);
		}

		const auto* map_ref = std::as_const(world->state()).ctx().find<inp::action_map_ref>();
		const auto* trace = std::as_const(world->state()).ctx().find<inp::action_trace>();
		// The first delivery the map was in use for; unknown (no trace of this map): name nothing.
		std::uint64_t map_since = UINT64_MAX;
		if (trace && map_ref && trace->map == map_ref->map) map_since = trace->map_since_delivery;

		json::object out;
		write_header(out, *at);
		out["world"] = safe_string(world->get_name());
		out["delivery"] = static_cast<std::int64_t>(bus->deliveries());
		out["journal_capacity"] = static_cast<std::int64_t>(journal->capacity());
		json::array events;
		events.reserve(picked.size());
		for (auto it = picked.rbegin(); it != picked.rend(); ++it) {
			const ecs::bus_journal_entry& entry = **it;
			json::object event;
			event["type"] = safe_string(entry.type);
			event["delivery"] = static_cast<std::int64_t>(entry.delivery);
			event["seq"] = static_cast<std::int64_t>(entry.seq);
			event["from"] = entry.from ? json::value(safe_string(*entry.from)) : json::value(nullptr);
			json::value payload = payload_of(entry.payload);
			if (type_matches(entry.type, "inp::action_event")) {
				name_action(payload, map_ref && map_ref->map ? map_ref->map.get() : nullptr, map_since, entry.delivery);
			}
			event["payload"] = std::move(payload);
			events.push_back(std::move(event));
		}
		out["events"] = std::move(events);
		return cmd::result::success(std::move(out));
	}

	json::object actions_of(const scn::world_context& world)
	{
		const entt::registry& reg = world.state();
		json::object out;
		out["world"] = safe_string(world.get_name());
		const auto* map_ref = reg.ctx().find<inp::action_map_ref>();
		const auto* state = reg.ctx().find<inp::action_state>();
		const auto* trace = reg.ctx().find<inp::action_trace>();
		const bool reads = map_ref && map_ref->map && state;
		out["reads_actions"] = reads;
		if (!reads) return out;
		out["updates"] = trace ? static_cast<std::int64_t>(trace->updates) : 0;
		const bool trace_current = trace && trace->map == map_ref->map;
		json::array actions;
		const auto listed = map_ref->map->actions();
		actions.reserve(listed.size());
		for (std::size_t id = 0; id < listed.size(); ++id) {
			const auto action = static_cast<inp::action_id>(id);
			json::object item;
			item["id"] = static_cast<std::int64_t>(id);
			item["name"] = safe_string(listed[id].name);
			item["held"] = state->held(action);
			item["pressed"] = state->pressed(action);
			item["released"] = state->released(action);
			json::value edge = nullptr;
			if (trace_current && id < trace->last_edges.size() && trace->last_edges[id]) {
				const auto& last = *trace->last_edges[id];
				edge = json::object{ { "update", static_cast<std::int64_t>(last.update) },
					{ "edge", last.kind == inp::action_edge::pressed ? "pressed" : "released" } };
			}
			item["last_edge"] = std::move(edge);
			actions.push_back(std::move(item));
		}
		out["actions"] = std::move(actions);
		return out;
	}

	json::array layers_of(const std::vector<inp::layer_reach>& layers)
	{
		json::array out;
		out.reserve(layers.size());
		for (const inp::layer_reach& layer : layers) {
			json::array keys;
			for (std::size_t key = 0; key < layer.keys.size(); ++key) {
				if (layer.keys.test(key)) keys.push_back(json::string(inp::key_code_name(static_cast<inp::KEYBOARD_BUTTONS>(key))));
			}
			out.push_back(json::object{ { "world", safe_string(layer.world) }, { "layer", safe_string(layer.layer) },
				{ "keys", std::move(keys) }, { "buttons", layer.buttons } });
		}
		return out;
	}

	cmd::result input_actions(const edt::bus_commands_deps& deps, const cmd::call& call)
	{
		const json::object& args = call.args.as_object();
		cmd::result error;
		const auto at = find_play(deps, args, error);
		if (!at) return error;

		json::object out;
		write_header(out, *at);
		json::array worlds;
		if (args.contains("world")) {
			scn::world_context* world = find_world(*at, args, error);
			if (!world) return error;
			worlds.push_back(actions_of(*world));
		} else {
			for (const std::string& name : at->play->world_names()) {
				if (const scn::world_context* world = at->set->find(name)) worlds.push_back(actions_of(*world));
			}
		}
		out["worlds"] = std::move(worlds);
		const inp::input_router* router = deps.input ? deps.input() : nullptr;
		const auto* layers = router ? router->last_layers(k_play_set) : nullptr;
		out["stack"] = layers ? json::value(layers_of(*layers)) : json::value(nullptr);
		return cmd::result::success(std::move(out));
	}

	void keep(std::vector<edt::registration>& out, edt::registration added)
	{
		if (added.active()) out.push_back(std::move(added));
	}

	constexpr std::string_view k_header_properties = R"json("source":{"type":"string","enum":["play"]},
		"play_state":{"type":"string","enum":["playing","paused"]},"frame":{"type":"integer"},
		"reload":{"type":"integer","description":"module reloads that rebuilt this Play's worlds; the journal starts again empty at each"})json";
}

std::vector<edt::registration> edt::register_bus_commands(cmd::command_registry& commands, bus_commands_deps deps)
{
	std::vector<registration> out;
	auto shared = std::make_shared<bus_commands_deps>(std::move(deps));

	cmd::command_desc events;
	events.id = "world.events";
	events.summary = "The last events the bus of a world of the running game (Play) delivered, oldest first: type, delivery, seq, sender and the payload as JSON (null when the type has no payload writer; a payload over 4 KiB is {truncated, bytes}). Read-only, for debugging; outside Play an error";
	events.category = cmd::agent_category::read;
	events.args_schema = parse_schema(std::format(R"json({{"type":"object","properties":{{
		"world":{{"type":"string","description":"world name; default the first world of Play"}},
		"set":{{"type":"string","enum":["play"],"description":"only the worlds of Play are shown"}},
		"type":{{"type":"string","description":"only this event type, at most {} bytes: its full name (inp::action_event) or the part after the last ::"}},
		"last":{{"type":"integer","minimum":1,"maximum":{},"description":"how many of the latest events, default {}"}}}},"additionalProperties":false}})json",
		k_max_type_filter, play_journal_capacity, k_default_last));
	events.result_schema = parse_schema(std::format(R"({{"type":"object","properties":{{{},
		"world":{{"type":"string"}},"delivery":{{"type":"integer","description":"the world's deliveries so far"}},"journal_capacity":{{"type":"integer"}},
		"events":{{"type":"array","items":{{"type":"object","properties":{{"type":{{"type":"string"}},"delivery":{{"type":"integer"}},"seq":{{"type":"integer"}},
		"from":{{"type":["string","null"],"description":"the sending world, empty for an external post; null for a type of a module built before the journal"}},
		"payload":{{}}}},"required":["type","delivery","seq","from","payload"]}}}}}},"required":["source","frame","world","events"]}})", k_header_properties));
	events.invoke = [shared](const cmd::call& call) { return world_events(*shared, call); };
	keep(out, commands.add(std::move(events)));

	cmd::command_desc actions;
	actions.id = "input.actions";
	actions.summary = "The game actions in the worlds of the running game (Play): which world reads them, each action's held/pressed/released this frame and its last edge (update number of inp::update_actions), and the input stack of the last frame top first with what each layer took. Read-only, for debugging; outside Play an error";
	actions.category = cmd::agent_category::read;
	actions.args_schema = parse_schema(R"({"type":"object","properties":{
		"world":{"type":"string","description":"only this world of Play; default every world"}},"additionalProperties":false})");
	actions.result_schema = parse_schema(std::format(R"({{"type":"object","properties":{{{},
		"worlds":{{"type":"array","items":{{"type":"object","properties":{{"world":{{"type":"string"}},"reads_actions":{{"type":"boolean"}},
		"updates":{{"type":"integer"}},"actions":{{"type":"array"}}}},"required":["world","reads_actions"]}}}},
		"stack":{{"type":["array","null"]}}}},"required":["source","frame","worlds"]}})", k_header_properties));
	actions.invoke = [shared](const cmd::call& call) { return input_actions(*shared, call); };
	keep(out, commands.add(std::move(actions)));
	return out;
}
