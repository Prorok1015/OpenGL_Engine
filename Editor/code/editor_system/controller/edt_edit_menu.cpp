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
	action_registry& actions, document_manager& documents, const dialog_service& dialogs, std::function<bool()> gesture_active,
	std::function<std::optional<std::string>()> asset_undo_label, std::function<bool()> asset_undo)
{
	const auto idle = [&dialogs, gesture_active = std::move(gesture_active)] {
		return dialogs.pending() == 0 && (!gesture_active || !gesture_active());
	};
	// A data window is the active document after it opens and has no history yet: it leaves Ctrl+Z to the scene. The
	// read-only prefab preview does not: while it is active there is nothing to undo.
	const auto undo_target = [&documents]() -> document* {
		document* current = documents.active();
		return current && (current->next_undo_entry() || current == documents.preview()) ? current : &documents.scene();
	};
	const auto redo_target = [&documents]() -> document* {
		document* current = documents.active();
		return current && (current->next_redo_entry() || current == documents.preview()) ? current : &documents.scene();
	};
	const auto accept_target = [&documents]() -> document* {
		document* current = documents.active();
		return current && current->has_agent_changes() ? current : &documents.scene();
	};
	std::vector<registration> out;

	out.push_back(actions.add({ .id = "edit.undo", .label = "Undo", .menu_path = "Edit", .shortcut = "Ctrl+Z",
		.dynamic_label = [undo_target] {
			const document* doc = undo_target();
			return menu_text("Undo", doc ? doc->next_undo_entry() : std::nullopt);
		},
		.yields_to_text_input = true,
		.execute = [undo_target](const json::value&) {
			document* doc = undo_target();
			const auto entry = doc ? doc->next_undo_entry() : std::nullopt;
			return action_result::success(applied(doc && doc->undo(), entry));
		},
		.result_schema = applied_schema(),
		.enabled = [undo_target, idle] { return idle() && undo_target() && !undo_target()->edits_locked() && undo_target()->next_undo_entry().has_value(); } }));

	out.push_back(actions.add({ .id = "edit.redo", .label = "Redo", .menu_path = "Edit", .shortcut = "Ctrl+Y",
		.dynamic_label = [redo_target] {
			const document* doc = redo_target();
			return menu_text("Redo", doc ? doc->next_redo_entry() : std::nullopt);
		},
		.yields_to_text_input = true,
		.execute = [redo_target](const json::value&) {
			document* doc = redo_target();
			const auto entry = doc ? doc->next_redo_entry() : std::nullopt;
			return action_result::success(applied(doc && doc->redo(), entry));
		},
		.result_schema = applied_schema(),
		.enabled = [redo_target, idle] { return idle() && redo_target() && !redo_target()->edits_locked() && redo_target()->next_redo_entry().has_value(); } }));

	// The asset fixes of Problems (S57-4) are undone only on purpose, never by Ctrl+Z: no shortcut; the person's own, so
	// no policy file hands it to an agent.
	if (asset_undo_label && asset_undo) {
		out.push_back(actions.add({ .id = "edit.undo_asset_action", .label = "Undo Asset Action", .menu_path = "Edit",
			.dynamic_label = [asset_undo_label] {
				const std::optional<std::string> label = asset_undo_label();
				return label ? "Undo Asset Action: " + *label : std::string{ "Undo Asset Action" };
			},
			.execute = [asset_undo_label, asset_undo](const json::value&) {
				const std::optional<std::string> label = asset_undo_label();
				const bool undone = label && asset_undo();
				json::object out{ { "applied", undone } };
				if (undone) {
					out["label"] = *label;
				}
				return action_result::success(std::move(out));
			},
			.result_schema = applied_schema(),
			.enabled = [idle, asset_undo_label] { return idle() && asset_undo_label().has_value(); },
			.category = cmd::agent_category::not_for_agents }));
	}

	// The person has looked at what the agent did: no more "changed by agent" marks. Not for agents themselves.
	out.push_back(actions.add({ .id = "edit.accept_agent_changes", .label = "Accept Agent Changes", .menu_path = "Edit",
		.separator_before = true,
		.execute = [accept_target](const json::value&) {
			document* doc = accept_target();
			const bool had = doc && doc->has_agent_changes();
			if (had) doc->accept_agent_changes();
			return action_result::success(json::object{ { "accepted", had } });
		},
		.result_schema = json::parse(R"({"type":"object","properties":{"accepted":{"type":"boolean"}},"required":["accepted"]})"),
		.enabled = [accept_target] { return accept_target() && accept_target()->has_agent_changes(); },
		.category = cmd::agent_category::not_for_agents }));

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
