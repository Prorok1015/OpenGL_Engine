#include "edt_edit_menu.h"
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace {
	namespace json = boost::json;

	const char* origin_name(cmd::origin_kind kind)
	{
		switch (kind) {
		case cmd::origin_kind::ui: return "ui";
		case cmd::origin_kind::test: return "test";
		case cmd::origin_kind::agent: return "agent";
		case cmd::origin_kind::game: return "game";
		}
		return "ui";
	}

	json::value applied_schema()
	{
		return json::parse(R"({"type":"object","properties":{"applied":{"type":"boolean"},"label":{"type":"string"},
			"origin":{"type":"string","enum":["ui","test","agent","game"],"description":"who made the edit"}},"required":["applied"]})");
	}

	json::value applied(bool done, const std::optional<edt::history_entry>& entry)
	{
		json::object out{ { "applied", done } };
		if (done && entry) {
			out["label"] = entry->label;
			out["origin"] = origin_name(entry->origin.kind);
		}
		return out;
	}

	// "Undo Rename node", "Undo Rename node (agent)", or just "Undo".
	std::string menu_text(const char* verb, const std::optional<edt::history_entry>& entry)
	{
		if (!entry) return verb;
		std::string text = std::string(verb) + " " + entry->label;
		if (entry->origin.kind == cmd::origin_kind::agent) text += " (agent)";
		return text;
	}
}

std::vector<edt::registration> edt::register_edit_actions(
	action_registry& actions, document_manager& documents, const dialog_service& dialogs, std::function<bool()> gesture_active)
{
	const auto idle = [&dialogs, gesture_active = std::move(gesture_active)] {
		return dialogs.pending() == 0 && (!gesture_active || !gesture_active());
	};
	const auto active = [&documents]() -> document* { return documents.active(); };
	std::vector<registration> out;

	out.push_back(actions.add({ .id = "edit.undo", .label = "Undo", .menu_path = "Edit", .shortcut = "Ctrl+Z",
		.dynamic_label = [active] {
			const document* doc = active();
			return menu_text("Undo", doc ? doc->next_undo_entry() : std::nullopt);
		},
		.yields_to_text_input = true,
		.execute = [active](const json::value&) {
			document* doc = active();
			const auto entry = doc ? doc->next_undo_entry() : std::nullopt;
			return action_result::success(applied(doc && doc->undo(), entry));
		},
		.result_schema = applied_schema(),
		.enabled = [active, idle] { return idle() && active() && active()->next_undo_entry().has_value(); } }));

	out.push_back(actions.add({ .id = "edit.redo", .label = "Redo", .menu_path = "Edit", .shortcut = "Ctrl+Y",
		.dynamic_label = [active] {
			const document* doc = active();
			return menu_text("Redo", doc ? doc->next_redo_entry() : std::nullopt);
		},
		.yields_to_text_input = true,
		.execute = [active](const json::value&) {
			document* doc = active();
			const auto entry = doc ? doc->next_redo_entry() : std::nullopt;
			return action_result::success(applied(doc && doc->redo(), entry));
		},
		.result_schema = applied_schema(),
		.enabled = [active, idle] { return idle() && active() && active()->next_redo_entry().has_value(); } }));

	// The person has looked at what the agent did: no more "changed by agent" marks. Not for agents themselves.
	out.push_back(actions.add({ .id = "edit.accept_agent_changes", .label = "Accept Agent Changes", .menu_path = "Edit",
		.separator_before = true,
		.execute = [active](const json::value&) {
			document* doc = active();
			const bool had = doc && doc->has_agent_changes();
			if (had) doc->accept_agent_changes();
			return action_result::success(json::object{ { "accepted", had } });
		},
		.result_schema = json::parse(R"({"type":"object","properties":{"accepted":{"type":"boolean"}},"required":["accepted"]})"),
		.enabled = [active] { return active() && active()->has_agent_changes(); } }));

	// The other usual binding of Redo: no menu item of its own.
	out.push_back(actions.add({ .id = "edit.redo_alt", .label = "Redo", .shortcut = "Ctrl+Shift+Z",
		.yields_to_text_input = true,
		.execute = [&actions](const json::value&) {
			const action_result redone = actions.invoke("edit.redo", json::object{});
			return redone.ok ? redone : action_result::success(applied(false, std::nullopt));
		},
		.result_schema = applied_schema(),
		.expose_as_tool = false,
		.enabled = [&actions] { return actions.is_enabled("edit.redo"); } }));
	return out;
}
