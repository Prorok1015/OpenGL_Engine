#pragma once
#include "edt_registration.h"
#include <snk/command/cmd_registry.h>
#include <boost/json/value.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace edt
{
	// A key with modifiers, e.g. "Ctrl+Shift+S". The key is a single letter or digit, F1-F12 or a name such as
	// "Delete"; it is stored upper-case. Modifiers must match exactly.
	struct shortcut
	{
		bool ctrl = false;
		bool shift = false;
		bool alt = false;
		std::string key;

		// Empty on a malformed text (no key, unknown modifier, two keys).
		static std::optional<shortcut> parse(std::string_view text);
		std::string to_string() const;
		bool operator==(const shortcut&) const = default;
	};

	// Outcome of invoking an action: the engine's command result. Codes: "ok", "invalid_args" (the arguments do not
	// match the schema), "disabled", "not_found", "needs_input" (a valid call that needs a decision first, e.g.
	// discard_unsaved), "failed" (the action ran and could not do its job) or "result_invalid" (it ran, but its
	// result breaks result_schema).
	using action_result = cmd::result;

	using action_fn = std::function<action_result(const boost::json::value& args)>;

	// JSON Schema of an action without arguments (and of a result that carries nothing).
	inline boost::json::value no_args_schema() { return cmd::no_args_schema(); }
	// Wraps a body that takes nothing and returns nothing.
	inline action_fn no_args(std::function<void()> body)
	{
		return [body = std::move(body)](const boost::json::value&) {
			body();
			return action_result::success();
		};
	}

	// What the editor adds to a command: how a person reaches it. A command without it (an engine or game one)
	// has no menu item and no shortcut.
	struct action_ui
	{
		std::string label;
		// Top-level menu the item lives in ("File"). Empty: no menu item, the action is reachable by shortcut and invoke() only.
		std::string menu_path;
		// Binding, parsed by shortcut::parse. Shown in the menu next to the label unless `shortcut_hint` is set.
		std::string shortcut;
		// Text shown in the menu instead of the binding (an item whose shortcut triggers another action).
		std::string shortcut_hint;
		bool separator_before = false;
		// What a menu click or the shortcut does: normally asks the user for the arguments (dialog_service)
		// and then calls registry.invoke. Null: invoke with no arguments, which an action with required
		// arguments cannot do, so it needs one to have a menu item or a shortcut.
		std::function<void()> activate;
	};

	// An action in one piece, the way the built-in menus write it: add() splits it into a cmd::command_desc and an
	// action_ui.
	struct action_desc
	{
		std::string id;            // "file.save_level"
		std::string label;
		std::string menu_path;
		std::string shortcut;
		std::string shortcut_hint;
		bool separator_before = false;
		// Everything the action needs comes in `args`; it never reads ImGui input. Dialogs belong to `activate`.
		action_fn execute;
		// JSON Schema (see cmd_json_schema.h) of the arguments (a top-level "type":"object") and of the result. Null: no_args_schema(). `invoke` validates the arguments
		// before it calls `execute` and the result after it.
		boost::json::value args_schema;
		boost::json::value result_schema;
		std::uint32_t schema_version = 1;
		// Listed by tools() for agents and scripts. False for placeholders that do nothing.
		bool expose_as_tool = true;
		// Changes the document, the files or the state of the editor (cmd::command_desc::mutates). True unless the
		// action says it only reads: the MCP server offers an agent the commands that do not mutate, so a forgotten
		// flag must keep an action out of reach, not put it in.
		bool mutates = true;
		std::function<void()> activate;
		// Null: always enabled.
		std::function<bool()> enabled;
	};

	// One menu item as the main menu draws it.
	struct menu_item_info
	{
		std::string id;
		std::string label;
		std::string shortcut_text;
		bool separator_before = false;
		bool enabled = true;
	};

	// The editor's view of a cmd::command_registry: the commands themselves (id, schemas, enabled, invoke) live in
	// the registry it owns; the menu, the shortcuts and `activate` are a side table keyed by id, in registration
	// order. A command is one registration: dropping the handle removes the command and its UI together.
	// Built-in actions go through the same add() as extensions. Main thread only; actions may add and remove
	// registrations while they run.
	class action_registry
	{
	public:
		// Owns a registry of its own.
		action_registry();
		// Adds the editor's part to a registry that outlives this object and is shared with the engine and the game
		// (they register log.read and the like into it). Commands registered there show up in tools() and invoke();
		// size() counts them too.
		explicit action_registry(cmd::command_registry& shared);
		~action_registry();
		action_registry(const action_registry&) = delete;
		action_registry& operator=(const action_registry&) = delete;

		// The engine-side registry. A command added to it directly runs through invoke() and tools() like any
		// other, but has no menu item or shortcut.
		cmd::command_registry& commands() { return m_commands; }
		const cmd::command_registry& commands() const { return m_commands; }

		// Declares a top-level menu so the menu order does not depend on which action registers first.
		// A menu that only actions mention follows the declared ones, in the order of its first action.
		[[nodiscard]] registration add_menu(std::string name);

		// Errors are logged and nothing is registered (the registration is not active): an empty id or label,
		// no execute, a taken id, a bad schema, a malformed shortcut, a menu_path with '/' (submenus are not supported).
		// A shortcut already bound to another action is allowed with a warning: the earlier one wins.
		[[nodiscard]] registration add(action_desc desc);
		// The command plus how a person reaches it (nullopt: no menu item, no shortcut). Same checks.
		[[nodiscard]] registration add(cmd::command_desc command, std::optional<action_ui> ui);

		bool contains(std::string_view id) const;
		std::size_t size() const { return m_commands.size(); }

		// Top-level menus in display order, including declared ones that have no action.
		std::vector<std::string> menus() const;
		// Items of one menu in registration order (state of `enabled` is read now).
		std::vector<menu_item_info> items(std::string_view menu) const;

		bool is_enabled(std::string_view id) const;
		const cmd::command_desc* find(std::string_view id) const;
		// The actions an agent may call, in registration order (expose_as_tool).
		std::vector<const cmd::command_desc*> tools() const;
		std::vector<std::string> ids() const;

		// The one way to run an action, for the UI, scripts and agents alike. Fails, and `execute` does not
		// run, if the action does not exist, is disabled or `args` do not match its schema (see action_result::code).
		action_result invoke(std::string_view id, const boost::json::value& args, cmd::origin from = cmd::origin::ui());
		action_result invoke(std::string_view id, const cmd::call& request);
		// What a menu click does: `activate`, or invoke with no arguments. False (nothing runs) if the action
		// does not exist or is disabled.
		bool activate(std::string_view id);

		// Activates the first enabled action bound to `chord`. False if there is none.
		bool dispatch(const shortcut& chord);
		// Visits every bound shortcut (not the ones of disabled actions); `visit` must not change the registry.
		void for_each_shortcut(const std::function<void(const shortcut&)>& visit) const;

	private:
		struct ui_entry;
		struct menu_decl;

		std::shared_ptr<ui_entry> find_ui(std::string_view id) const;

		std::unique_ptr<cmd::command_registry> m_owned;
		cmd::command_registry& m_commands;
		std::vector<std::shared_ptr<ui_entry>> m_ui;
		std::vector<std::shared_ptr<menu_decl>> m_menus;
		std::uint64_t m_next_key = 1;
		std::shared_ptr<int> m_alive = std::make_shared<int>(0);
	};
}
