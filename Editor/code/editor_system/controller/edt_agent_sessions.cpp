#include "edt_agent_sessions.h"
#include "edt_scene_document.h"
#include "edt_scene_history.h"
#include <snk/editor/edt_document_manager.h>
#include <snk/common/engine_assert.h>
#include <snk/desc/desc_node_id.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <utility>

namespace {
	namespace json = boost::json;
	using prefab_node = scn::prefab_desc::prefab_node;

	json::value parse_schema(std::string_view text)
	{
		boost::system::error_code ec;
		json::value parsed = json::parse(text, ec);
		ASSERT_MSG(!ec, "an agent.session schema is not JSON: {}", ec.message());
		return parsed;
	}

	// The table is keyed by who calls: each MCP session has its own; scripts and tests share one per origin kind.
	std::string key_of(const cmd::origin& from)
	{
		switch (from.kind) {
		case cmd::origin_kind::agent: return "agent:" + from.session;
		case cmd::origin_kind::test: return "test";
		case cmd::origin_kind::game: return "game";
		case cmd::origin_kind::ui: return "ui";
		}
		return "ui";
	}

	bool path_to(const std::vector<prefab_node>& nodes, std::uint64_t id, std::vector<std::uint64_t>& path)
	{
		for (const auto& node : nodes) {
			path.push_back(node.node_id);
			if (node.node_id == id || path_to(node.children, id, path)) return true;
			path.pop_back();
		}
		return false;
	}

	// The ref of the node a conflict key names, as node.get takes it; nothing when the node is no longer there.
	std::optional<json::object> ref_of(const edt::scene_document& scene, const std::string& key)
	{
		const auto node = edt::parse_node_key(key);
		if (!node || node->first >= scene.world_descs.size() || node->first >= scene.world_names.size()) return std::nullopt;
		std::vector<std::uint64_t> path;
		if (!path_to(scene.world_descs[node->first].get_root().children, node->second, path)) return std::nullopt;
		json::array ids;
		for (const std::uint64_t id : path) ids.emplace_back(json::string(desc::format_node_id(id)));
		return json::object{ { "context", json::string(scene.world_names[node->first]) }, { "path", std::move(ids) } };
	}

	constexpr std::string_view no_args = R"({"type":"object","properties":{},"additionalProperties":false})";
	constexpr std::string_view state_result = R"({"type":"object","properties":{
		"open":{"type":"boolean","description":"the caller has a session open now"},
		"label":{"type":"string"},
		"group":{"type":"integer","description":"the history group of the current step"},
		"steps":{"type":"integer","description":"steps of the session (checkpoints + 1)"},
		"records":{"type":"integer","description":"undo records of the session in the history"}},"required":["open"]})";
}

std::vector<edt::registration> edt::agent_sessions::register_commands(cmd::command_registry& commands)
{
	std::vector<registration> out;
	const auto add = [&](std::string id, std::string summary, std::string_view args, std::string_view result, bool mutates,
		cmd::result (agent_sessions::*body)(const cmd::call&)) {
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.summary = std::move(summary);
		desc.args_schema = parse_schema(args);
		desc.result_schema = parse_schema(result);
		desc.mutates = mutates;
		desc.category = cmd::agent_category::scene_edit;
		desc.invoke = [this, body](const cmd::call& call) { return (this->*body)(call); };
		registration added = commands.add(std::move(desc));
		if (added.active()) out.push_back(std::move(added));
	};

	add("agent.session.begin", "Open a session: the caller's edits of the scene become one undo record per step, labelled `label`, that agent.session.rollback can undo whole (an open session of the caller is ended first)",
		R"({"type":"object","properties":{"label":{"type":"string","minLength":1}},"required":["label"],"additionalProperties":false})",
		state_result, true, &agent_sessions::begin);
	add("agent.session.checkpoint", "End the current step of the session and start the next one (its own undo record); rollback still undoes all steps",
		R"({"type":"object","properties":{"label":{"type":"string","minLength":1,"description":"the next step's; default: the session's"}},"additionalProperties":false})",
		state_result, true, &agent_sessions::checkpoint);
	add("agent.session.end", "Close the session; its edits stay as they are (and can still be undone one record at a time)",
		no_args, state_result, true, &agent_sessions::end);
	add("agent.session.rollback", "Undo the session's edits, newest first; stops before an edit whose node someone else (a person, another session) changed after it (stopped_at) and then leaves the session open",
		no_args,
		R"({"type":"object","properties":{
			"rolled_back":{"type":"integer"},"remaining":{"type":"integer","description":"edits still applied: the rollback stopped before them"},
			"unavailable":{"type":"integer","description":"edits that went over the history limit and stay applied"},
			"open":{"type":"boolean","description":"the session is still open (it stopped at a conflict)"},
			"failed":{"type":"boolean","description":"undoing threw: the history was cleared"},
			"stopped_at":{"type":"object","properties":{"key":{"type":"string"},"label":{"type":"string","description":"the label of the later edit in the way: a person's, or a step of another session"},
				"ref":{"type":"object","description":"the node, as node.get takes it"}},"required":["key"]}},
			"required":["rolled_back","remaining","open"]})",
		true, &agent_sessions::rollback);
	add("agent.session.get", "The caller's session: open, label, steps and undo records", no_args, state_result, false, &agent_sessions::get);
	return out;
}

edt::agent_sessions::session* edt::agent_sessions::find(const cmd::origin& from)
{
	const auto it = m_sessions.find(key_of(from));
	if (it == m_sessions.end()) return nullptr;
	// A boundary of the history (a level loaded, a world added) forgot the groups: the session is over.
	if (it->second.groups.empty() || m_documents.scene().history().open_group(from) != it->second.groups.back()) {
		m_sessions.erase(it);
		return nullptr;
	}
	return &it->second;
}

void edt::agent_sessions::end_session(const std::string& key, const cmd::origin& from)
{
	const auto it = m_sessions.find(key);
	if (it == m_sessions.end()) return;
	auto& history = m_documents.scene().history();
	if (!it->second.groups.empty() && history.open_group(from) == it->second.groups.back()) {
		history.end_group(it->second.groups.back());
	}
	m_sessions.erase(it);
}

void edt::agent_sessions::session_closed(const std::string& session_id)
{
	end_session(key_of(cmd::origin::agent(session_id)), cmd::origin::agent(session_id));
}

cmd::result edt::agent_sessions::begin(const cmd::call& call)
{
	// A person's (or a script's) edits in a session would be undone by its rollback without anyone seeing it coming.
	if (call.from.kind != cmd::origin_kind::agent && call.from.kind != cmd::origin_kind::test) {
		return cmd::result::failure(cmd::codes::denied, "a session is for agents: the caller is not one");
	}
	const std::string key = key_of(call.from);
	end_session(key, call.from);
	session opened;
	opened.label = std::string(call.args.as_object().at("label").as_string());
	opened.groups.push_back(m_documents.scene().history().begin_group(opened.label, call.from));
	m_sessions[key] = opened;
	return get(call);
}

cmd::result edt::agent_sessions::checkpoint(const cmd::call& call)
{
	session* current = find(call.from);
	if (!current) return cmd::result::failure(cmd::codes::failed, "no session is open: agent.session.begin first");
	auto& history = m_documents.scene().history();
	history.end_group(current->groups.back());
	const json::value* label = call.args.as_object().if_contains("label");
	current->groups.push_back(history.begin_group(label ? std::string(label->as_string()) : current->label, call.from));
	return get(call);
}

cmd::result edt::agent_sessions::end(const cmd::call& call)
{
	if (!find(call.from)) return cmd::result::failure(cmd::codes::failed, "no session is open");
	end_session(key_of(call.from), call.from);
	return get(call);
}

cmd::result edt::agent_sessions::rollback(const cmd::call& call)
{
	session* current = find(call.from);
	if (!current) return cmd::result::failure(cmd::codes::failed, "no session is open");
	scene_document& scene = m_documents.scene();

	json::object out;
	std::size_t rolled_back = 0;
	std::size_t remaining = 0;
	std::size_t unavailable = 0;
	bool failed = false;
	std::optional<rollback_result> stopped;
	// Newest step first; a step undone whole leaves the list.
	while (!current->groups.empty()) {
		const rollback_result step = scene.rollback_group(current->groups.back());
		rolled_back += step.rolled_back;
		remaining += step.remaining;
		unavailable += step.unavailable;
		if (step.failed) {
			failed = true;
			break;
		}
		if (step.conflict_key) {
			stopped = step;
			break;
		}
		current->groups.pop_back();
	}

	if (stopped) {
		// The steps before the one it stopped in were not reached: all of their edits are still there.
		const auto& history = scene.history();
		for (std::size_t g = 0; g + 1 < current->groups.size(); ++g) {
			for (const history_record& record : history.records()) {
				if (record.group == current->groups[g]) {
					remaining += record.changes.size();
				}
			}
			unavailable += history.evicted(current->groups[g]);
		}
	}
	out["rolled_back"] = rolled_back;
	out["remaining"] = remaining;
	out["unavailable"] = unavailable;
	if (failed) {
		// The history is gone, and the session with it.
		m_sessions.erase(key_of(call.from));
		out["failed"] = true;
		out["open"] = false;
		return cmd::result::success(std::move(out));
	}
	if (!stopped) {
		m_sessions.erase(key_of(call.from));
		out["open"] = false;
		return cmd::result::success(std::move(out));
	}

	// Still open: the person's edit is in the way; the agent may end the session or roll back again later. The steps
	// above the conflict were closed by their rollback, so the edits from now on go into a step of their own.
	auto& history = scene.history();
	if (history.open_group(call.from) != current->groups.back()) {
		current->groups.push_back(history.begin_group(current->label, call.from));
	}
	json::object at{ { "key", json::string(*stopped->conflict_key) }, { "label", json::string(stopped->conflict_label) } };
	if (auto ref = ref_of(scene, *stopped->conflict_key)) at["ref"] = std::move(*ref);
	out["stopped_at"] = std::move(at);
	out["open"] = true;
	return cmd::result::success(std::move(out));
}

cmd::result edt::agent_sessions::get(const cmd::call& call)
{
	const session* current = find(call.from);
	if (!current) return cmd::result::success(json::object{ { "open", false } });
	std::size_t records = 0;
	for (const history_record& record : m_documents.scene().history().records()) {
		if (std::ranges::find(current->groups, record.group) != current->groups.end()) ++records;
	}
	return cmd::result::success(json::object{
		{ "open", true }, { "label", json::string(current->label) }, { "group", current->groups.back() },
		{ "steps", current->groups.size() }, { "records", records } });
}
