#include "edt_file_menu.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_project.h>
#include <format>
#include <string_view>
#include <utility>

namespace json = boost::json;

namespace {
	constexpr const char* DEFAULT_LEVEL_PATH = "levels/my_level.desc";

	// {name: non-empty string}, plus the optional discard_unsaved flag when `with_discard`.
	json::value path_schema(bool with_discard)
	{
		json::object properties{ { "path", json::object{ { "type", "string" }, { "minLength", 1 } } } };
		if (with_discard) properties["discard_unsaved"] = json::object{ { "type", "boolean" } };
		return json::object{
			{ "type", "object" },
			{ "properties", std::move(properties) },
			{ "required", json::array{ "path" } },
			{ "additionalProperties", false } };
	}

	json::value discard_schema()
	{
		return json::object{
			{ "type", "object" },
			{ "properties", json::object{ { "discard_unsaved", json::object{ { "type", "boolean" } } } } },
			{ "additionalProperties", false } };
	}

	// Paths cross the JSON boundary as UTF-8.
	std::string to_json_string(const std::filesystem::path& path)
	{
		const std::u8string text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::filesystem::path from_json_string(const json::string& text)
	{
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
	}

	bool wants_discard(const json::value& args)
	{
		const json::value* flag = args.as_object().if_contains("discard_unsaved");
		return flag && flag->as_bool();
	}

	constexpr const char* UNSAVED_MESSAGE = "the level has unsaved changes: pass discard_unsaved to drop them";
	constexpr std::string_view LEVEL_NOT_SAVED = "The level was not saved (the changes are kept)";
	constexpr std::string_view PROJECT_NOT_OPENED = "The project was not opened";

	// {path, allow_engine_version_mismatch?, discard_unsaved?}: what file.open_project takes.
	json::value open_project_schema()
	{
		return json::object{
			{ "type", "object" },
			{ "properties", json::object{
				{ "path", json::object{ { "type", "string" }, { "minLength", 1 } } },
				{ "allow_engine_version_mismatch", json::object{ { "type", "boolean" } } },
				{ "discard_unsaved", json::object{ { "type", "boolean" } } } } },
			{ "required", json::array{ "path" } },
			{ "additionalProperties", false } };
	}

	// {path, name?, discard_unsaved?}: what file.new_project takes; the name defaults to the folder's name.
	json::value new_project_schema()
	{
		return json::object{
			{ "type", "object" },
			{ "properties", json::object{
				{ "path", json::object{ { "type", "string" }, { "minLength", 1 } } },
				{ "name", json::object{ { "type", "string" }, { "minLength", 1 } } },
				{ "discard_unsaved", json::object{ { "type", "boolean" } } } } },
			{ "required", json::array{ "path" } },
			{ "additionalProperties", false } };
	}

	bool flag(const json::value& args, std::string_view key)
	{
		const json::value* value = args.as_object().if_contains(key);
		return value && value->as_bool();
	}
}

edt::file_menu::file_menu(dialog_service& dialogs, action_registry& actions, file_menu_hooks hooks)
	: m_dialogs(dialogs)
	, m_actions(actions)
	, m_hooks(std::move(hooks))
{
}

bool edt::file_menu::invoke(std::string_view action_id, json::value args)
{
	const action_result outcome = m_actions.invoke(action_id, args);
	if (!outcome.ok) egLOG_WARN("Editor/File", "{}", outcome.error);
	return outcome.ok;
}

bool edt::file_menu::invoke_or_report(std::string_view action_id, json::value args, std::string_view failure)
{
	const action_result outcome = m_actions.invoke(action_id, args);
	if (!outcome.ok) {
		egLOG_WARN("Editor/File", "{}", outcome.error);
		m_dialogs.message(message_level::error, std::format("{}: {}. The console has the cause.", failure, outcome.error));
	}
	return outcome.ok;
}

std::optional<edt::dialog_service::dialog_id> edt::file_menu::guard_unsaved(std::function<void(bool discard)> action)
{
	if (!m_hooks.is_dirty()) {
		action(false);
		return std::nullopt;
	}
	return m_dialogs.confirm_unsaved([this, alive = std::weak_ptr<void>(m_alive), action = std::move(action)](unsaved_choice choice) {
		if (alive.lock()) on_unsaved_choice(choice, action);
	});
}

void edt::file_menu::on_unsaved_choice(unsaved_choice choice, const std::function<void(bool discard)>& action)
{
	switch (choice) {
	case unsaved_choice::save:
		if (!m_hooks.has_scene()) {
			egLOG("Editor/Save", "No scene loaded - nothing to save.");
			return;
		}
		if (!m_hooks.level_path().empty()) {
			// A failed write must not continue into New/Exit: the changes would be lost.
			if (invoke_or_report("file.save", json::object{}, LEVEL_NOT_SAVED)) action(true);
			return;
		}
		// Untitled level: Save-As runs first, the action follows a successful write.
		ask_save_path([action] { action(true); });
		return;
	case unsaved_choice::discard:
		action(true);
		return;
	case unsaved_choice::cancel:
		return;
	}
}

void edt::file_menu::new_level()
{
	guard_unsaved([this](bool discard) { invoke("file.new", json::object{ { "discard_unsaved", discard } }); });
}

void edt::file_menu::exit()
{
	// A second close request (the X clicked again) must not stack another prompt behind the first, nor
	// one on top of a Save As that the first prompt's "Save" opened.
	if (m_exit_prompt_id && m_dialogs.is_pending(*m_exit_prompt_id)) return;
	if (m_save_as_id && m_dialogs.is_pending(*m_save_as_id)) return;
	m_exit_prompt_id = guard_unsaved([this](bool discard) { invoke("file.exit", json::object{ { "discard_unsaved", discard } }); });
}

void edt::file_menu::attach_window_close(wnd::window_system& windows)
{
	windows.set_close_request_handler([this, alive = std::weak_ptr<void>(m_alive)] {
		if (alive.lock()) exit();
	});
}

void edt::file_menu::open_level()
{
	pick_file_options options;
	options.title = "Load Level";
	options.start_path = m_hooks.resources_path;
	options.extensions = { ".desc" };
	m_dialogs.pick_file(std::move(options), [this, alive = std::weak_ptr<void>(m_alive)](const std::filesystem::path& picked) {
		// Opening a level has never asked about unsaved changes in the UI, so the click passes them on.
		if (alive.lock()) invoke("file.open", json::object{ { "path", to_json_string(picked) }, { "discard_unsaved", true } });
	});
}

void edt::file_menu::import_model()
{
	pick_file_options options;
	options.title = "Import Model";
	options.start_path = std::filesystem::current_path();
	options.extensions = { ".glb", ".obj", ".fbx", ".gltf" };
	m_dialogs.pick_file(std::move(options), [this, alive = std::weak_ptr<void>(m_alive)](const std::filesystem::path& picked) {
		if (alive.lock()) invoke("file.import_model", json::object{ { "path", to_json_string(picked) } });
	});
}

void edt::file_menu::save_level()
{
	if (!m_hooks.has_scene()) {
		egLOG("Editor/Save", "No scene loaded - nothing to save.");
		return;
	}
	ask_save_path({});
}

void edt::file_menu::ask_save_path(std::function<void()> then)
{
	// Ctrl+S is not blocked by a modal: a second request while one is open joins it instead of
	// opening another dialog that would write again with a stale path.
	const bool open_already = m_save_as_id && m_dialogs.is_pending(*m_save_as_id);
	if (!open_already) m_after_save.clear(); // left over from a dialog that was cancelled
	if (then) m_after_save.push_back(std::move(then));
	if (open_already) return;

	std::string initial = m_hooks.level_path();
	if (initial.empty()) initial = DEFAULT_LEVEL_PATH;
	m_save_as_id = m_dialogs.save_as(std::move(initial), [this, alive = std::weak_ptr<void>(m_alive)](const std::string& path) {
		if (!alive.lock()) return;
		auto after = std::move(m_after_save);
		m_after_save.clear();
		if (!invoke_or_report("file.save_as", json::object{ { "path", path } }, LEVEL_NOT_SAVED)) return;
		for (const auto& action : after) action();
	});
}

edt::action_result edt::file_menu::switch_to_project(const std::filesystem::path& project, bool allow_engine_version_mismatch)
{
	// Checked here, so a project that cannot open never costs the user this editor.
	const auto opened = prj::open_project(project);
	if (!opened.context) return action_result::failure("failed", opened.error);
	if (auto error = m_hooks.open_project(opened.context->manifest_file, allow_engine_version_mismatch))
		return action_result::failure("failed", *error);
	if (m_hooks.is_dirty()) m_hooks.discard_changes();
	m_hooks.exit();
	return action_result::success(json::object{ { "manifest", to_json_string(opened.context->manifest_file) } });
}

void edt::file_menu::new_project()
{
	const std::string initial = to_json_string(m_hooks.projects_path / "NewProject");
	m_dialogs.ask_path("New Project", "Folder of the new project (it is created; its name names the project):", initial,
		[this, alive = std::weak_ptr<void>(m_alive)](const std::string& path) {
			if (!alive.lock()) return;
			guard_unsaved([this, path](bool discard) {
				invoke_or_report("file.new_project", json::object{ { "path", path }, { "discard_unsaved", discard } },
					"The project was not created");
			});
		});
}

void edt::file_menu::open_project()
{
	pick_file_options options;
	options.title = "Open Project";
	options.start_path = m_hooks.projects_path;
	options.extensions = { std::string(prj::k_manifest_file_name.substr(prj::k_manifest_file_name.find('.'))) };
	m_dialogs.pick_file(std::move(options), [this, alive = std::weak_ptr<void>(m_alive)](const std::filesystem::path& picked) {
		if (!alive.lock()) return;
		guard_unsaved([this, picked](bool discard) {
			invoke_or_report("file.open_project", json::object{ { "path", to_json_string(picked) }, { "discard_unsaved", discard } },
				PROJECT_NOT_OPENED);
		});
	});
}

void edt::file_menu::quick_save()
{
	if (!m_hooks.has_scene()) return;
	if (!m_hooks.level_path().empty()) {
		invoke_or_report("file.save", json::object{}, LEVEL_NOT_SAVED);
	} else {
		save_level();
	}
}

void edt::file_menu::register_actions()
{
	const auto add = [this](action_desc desc) {
		m_registrations.push_back(m_actions.add(std::move(desc)));
	};

	add({ .id = "file.new", .label = "New Level", .menu_path = "File",
		.execute = [this](const json::value& args) {
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			if (m_hooks.is_dirty()) m_hooks.discard_changes();
			m_hooks.new_level();
			return action_result::success();
		},
		.args_schema = discard_schema(),
		.activate = [this] { new_level(); } });

	add({ .id = "file.open", .label = "Open Level...", .menu_path = "File",
		.execute = [this](const json::value& args) {
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			m_hooks.load_level(from_json_string(args.as_object().at("path").as_string()));
			return action_result::success();
		},
		.args_schema = path_schema(true),
		.activate = [this] { open_level(); } });

	add({ .id = "file.save", .label = "Save Level", .menu_path = "File", .shortcut = "Ctrl+S",
		.execute = [this](const json::value&) {
			if (!m_hooks.has_scene()) return action_result::failure("failed", "no level is loaded");
			if (m_hooks.level_path().empty()) return action_result::failure("failed", "the level has no path yet: use file.save_as");
			if (!m_hooks.quick_save()) return action_result::failure("failed", "the level could not be written");
			return action_result::success();
		},
		.activate = [this] { quick_save(); } });

	add({ .id = "file.save_as", .label = "Save Level As...", .menu_path = "File", .shortcut = "Ctrl+Shift+S",
		.execute = [this](const json::value& args) {
			if (!m_hooks.has_scene()) return action_result::failure("failed", "no level is loaded");
			if (!m_hooks.save_level_as(std::string(args.as_object().at("path").as_string())))
				return action_result::failure("failed", "the level could not be written");
			return action_result::success();
		},
		.args_schema = path_schema(false),
		.activate = [this] { save_level(); } });

	add({ .id = "file.import_model", .label = "Import Model...", .menu_path = "File", .separator_before = true,
		.disabled_tooltip = "Model import is not available: this editor was built without assimp (SNK_WITH_ASSIMP=OFF).",
		.execute = [this](const json::value& args) {
			if (!m_hooks.import_model) return action_result::failure("failed", "model import is not built in");
			m_hooks.import_model(from_json_string(args.as_object().at("path").as_string()));
			return action_result::success();
		},
		.args_schema = path_schema(false),
		.activate = [this] { import_model(); },
		.enabled = [this] { return static_cast<bool>(m_hooks.import_model); } });

	add({ .id = "file.new_project", .label = "New Project...", .menu_path = "File", .separator_before = true,
		.execute = [this](const json::value& args) {
			if (!m_hooks.create_project || !m_hooks.open_project) return action_result::failure("failed", "projects are not available here");
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			const auto folder = from_json_string(args.as_object().at("path").as_string()).lexically_normal();
			const auto base = folder.has_filename() ? folder.filename() : folder.parent_path().filename();
			const json::value* name = args.as_object().if_contains("name");
			const auto created = m_hooks.create_project(folder, name ? std::string(name->as_string()) : to_json_string(base));
			if (!created.error.empty()) return action_result::failure("failed", created.error);
			return switch_to_project(created.manifest_file, false);
		},
		.args_schema = new_project_schema(),
		.activate = [this] { new_project(); } });

	add({ .id = "file.open_project", .label = "Open Project...", .menu_path = "File",
		.execute = [this](const json::value& args) {
			if (!m_hooks.open_project) return action_result::failure("failed", "projects are not available here");
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			return switch_to_project(from_json_string(args.as_object().at("path").as_string()), flag(args, "allow_engine_version_mismatch"));
		},
		.args_schema = open_project_schema(),
		.activate = [this] { open_project(); } });

	add({ .id = "file.exit", .label = "Exit", .menu_path = "File", .separator_before = true,
		.execute = [this](const json::value& args) {
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			if (m_hooks.is_dirty()) m_hooks.discard_changes();
			m_hooks.exit();
			return action_result::success();
		},
		.args_schema = discard_schema(),
		.activate = [this] { exit(); } });
}
