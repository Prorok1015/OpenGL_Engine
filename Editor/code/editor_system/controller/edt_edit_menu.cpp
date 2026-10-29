#include "edt_edit_menu.h"
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_document_manager.h>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace {
	namespace json = boost::json;

	json::value applied_schema()
	{
		return json::parse(R"({"type":"object","properties":{"applied":{"type":"boolean"},"label":{"type":"string"}},
			"required":["applied"]})");
	}

	json::value applied(bool done, const std::optional<std::string>& label)
	{
		json::object out{ { "applied", done } };
		if (label) out["label"] = *label;
		return out;
	}
}

std::vector<edt::registration> edt::register_edit_actions(
	action_registry& actions, document_manager& documents, std::function<bool()> gesture_active)
{
	const auto idle = [gesture_active = std::move(gesture_active)] { return !gesture_active || !gesture_active(); };
	const auto active = [&documents]() -> document* { return documents.active(); };
	std::vector<registration> out;

	out.push_back(actions.add({ .id = "edit.undo", .label = "Undo", .menu_path = "Edit", .shortcut = "Ctrl+Z",
		.dynamic_label = [active] {
			const document* doc = active();
			const auto label = doc ? doc->next_undo_label() : std::nullopt;
			return label ? "Undo " + *label : std::string{ "Undo" };
		},
		.yields_to_text_input = true,
		.execute = [active](const json::value&) {
			document* doc = active();
			const auto label = doc ? doc->next_undo_label() : std::nullopt;
			const bool done = doc && doc->undo();
			return action_result::success(applied(done, done ? label : std::nullopt));
		},
		.result_schema = applied_schema(),
		.enabled = [active, idle] { return idle() && active() && active()->next_undo_label().has_value(); } }));

	out.push_back(actions.add({ .id = "edit.redo", .label = "Redo", .menu_path = "Edit", .shortcut = "Ctrl+Y",
		.dynamic_label = [active] {
			const document* doc = active();
			const auto label = doc ? doc->next_redo_label() : std::nullopt;
			return label ? "Redo " + *label : std::string{ "Redo" };
		},
		.yields_to_text_input = true,
		.execute = [active](const json::value&) {
			document* doc = active();
			const auto label = doc ? doc->next_redo_label() : std::nullopt;
			const bool done = doc && doc->redo();
			return action_result::success(applied(done, done ? label : std::nullopt));
		},
		.result_schema = applied_schema(),
		.enabled = [active, idle] { return idle() && active() && active()->next_redo_label().has_value(); } }));

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
