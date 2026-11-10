#include "edt_file_menu.h"
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_project.h>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>

namespace json = boost::json;

namespace {
	constexpr const char* DEFAULT_LEVEL_PATH = "levels/my_level.desc";

	// {name: non-empty string}, plus the optional discard_unsaved flag when `with_discard`.
	json::value path_schema(bool with_discard, std::string_view path_description = {})
	{
		json::object properties{ { "path", json::object{ { "type", "string" }, { "minLength", 1 } } } };
		if (!path_description.empty()) properties["path"].as_object()["description"] = json::string(path_description);
		if (with_discard) {
			properties["discard_unsaved"] = json::object{ { "type", "boolean" },
				{ "description", "Drop the level's unsaved changes. Only for a person; an agent saves first" } };
		}
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

	std::filesystem::path from_utf8(std::string_view text)
	{
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
	}

	bool is_reserved_device_name(std::string_view segment)
	{
		const std::string_view stem = segment.substr(0, segment.find('.'));
		std::string upper;
		upper.reserve(stem.size());
		for (const char character : stem) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
		while (!upper.empty() && upper.back() == ' ') upper.pop_back();
		if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
		return upper.size() == 4 && (upper.starts_with("COM") || upper.starts_with("LPT")) && upper[3] >= '0' && upper[3] <= '9';
	}

	bool wants_discard(const json::value& args)
	{
		const json::value* flag = args.as_object().if_contains("discard_unsaved");
		return flag && flag->as_bool();
	}

	constexpr const char* UNSAVED_MESSAGE = "the level has unsaved changes: pass discard_unsaved to drop them";
	constexpr std::string_view LEVEL_PATH_DESCRIPTION = "Level file inside the project: res://levels/a.desc or levels/a.desc (a *.desc "
		"path without '..', a drive or a link on the way). An agent may not overwrite another existing file";
	constexpr const char* AGENT_UNSAVED_MESSAGE = "the level has unsaved changes: save it first (file.save or file.save_as); "
		"dropping unsaved changes is done by a person in the editor";
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

	// What file.new_project and file.open_project give back: the manifest the new editor opens.
	json::value switched_project_schema()
	{
		return json::object{
			{ "type", "object" },
			{ "properties", json::object{ { "manifest", json::object{ { "type", "string" } } } } },
			{ "required", json::array{ "manifest" } },
			{ "additionalProperties", false } };
	}

	bool flag(const json::value& args, std::string_view key)
	{
		const json::value* value = args.as_object().if_contains(key);
		return value && value->as_bool();
	}
}

edt::level_file_state edt::inspect_level_file(const std::filesystem::path& root, const std::string& relative_path)
{
	if (root.empty()) return level_file_state::missing;
	std::filesystem::path current = root;
	std::size_t start = 0;
	while (start <= relative_path.size()) {
		const std::size_t end = relative_path.find('/', start);
		const bool last = end == std::string::npos;
		current /= from_utf8(std::string_view(relative_path).substr(start, last ? std::string_view::npos : end - start));
		std::error_code ec;
		const std::filesystem::file_status status = std::filesystem::symlink_status(current, ec);
		if (ec || status.type() == std::filesystem::file_type::not_found) return level_file_state::missing;
		if (last) return status.type() == std::filesystem::file_type::regular ? level_file_state::file : level_file_state::unsafe;
		if (status.type() != std::filesystem::file_type::directory) return level_file_state::unsafe;
		start = end + 1;
	}
	return level_file_state::missing;
}

namespace {
	// The checks of a project path that hold for a file and for a folder; empty text when the path is fine. `text` is
	// already without a `res://` prefix.
	std::string project_path_problem(std::string_view text)
	{
		if (text.empty()) return "the path is empty";
		if (text.size() > 512) return "the path is too long";
		if (text.find(':') != std::string_view::npos) return "the path must be a res:// tag or a path inside the project, without another protocol or a drive";
		if (text.find('\\') != std::string_view::npos) return "the path must use '/' as the separator";
		if (text.front() == '/') return "the path must be inside the project, not absolute";
		for (const char character : text) {
			if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) return "the path has a control character";
			if (std::string_view("<>\"|?*").find(character) != std::string_view::npos) return "the path has a character a file name cannot have";
		}
		std::size_t start = 0;
		while (true) {
			const std::size_t end = text.find('/', start);
			const std::string_view segment = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
			if (segment.empty() || segment == "." || segment == "..") return "the path must not have empty, '.' or '..' segments";
			// Windows drops a trailing dot or space and opens a device for CON, NUL, COM1... (with any extension).
			if (segment.back() == '.' || segment.back() == ' ') return "a name must not end in '.' or ' '";
			if (is_reserved_device_name(segment)) return "the path has a name Windows reserves for a device";
			if (end == std::string_view::npos) break;
			start = end + 1;
		}
		return {};
	}
}

edt::agent_level_path edt::check_agent_folder(std::string_view text)
{
	constexpr std::string_view scheme = "res://";
	if (text.starts_with(scheme)) text.remove_prefix(scheme.size());
	if (std::string why = project_path_problem(text); !why.empty()) return { {}, std::move(why) };
	return { std::string(text), {} };
}

edt::agent_level_path edt::check_agent_name(std::string_view text)
{
	if (text.find('/') != std::string_view::npos) return { {}, "a name is one path segment, without '/'" };
	if (std::string why = project_path_problem(text); !why.empty()) return { {}, std::move(why) };
	return { std::string(text), {} };
}

edt::agent_level_path edt::check_agent_level_path(std::string_view text)
{
	constexpr std::string_view scheme = "res://";
	constexpr std::string_view extension = ".desc";
	const auto refuse = [](std::string why) { return agent_level_path{ {}, std::move(why) }; };
	if (text.starts_with(scheme)) text.remove_prefix(scheme.size());
	if (std::string why = project_path_problem(text); !why.empty()) return refuse(std::move(why));
	const std::string_view name = text.substr(text.rfind('/') == std::string_view::npos ? 0 : text.rfind('/') + 1);
	if (name.size() <= extension.size() || !name.ends_with(extension)) return refuse("the path must name a level file (*.desc)");
	return { std::string(text), {} };
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

	const auto level_change_busy = [this]() -> std::optional<action_result> {
		if (!m_hooks.level_change_blocker) return std::nullopt;
		if (std::optional<std::string> why = m_hooks.level_change_blocker()) return action_result::failure(cmd::codes::busy, *why);
		return std::nullopt;
	};

	add({ .id = "file.new", .label = "New Level", .menu_path = "File",
		.execute = [this, level_change_busy](const json::value& args) {
			if (auto busy = level_change_busy()) return *busy;
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			if (m_hooks.is_dirty()) m_hooks.discard_changes();
			m_hooks.new_level();
			return action_result::success();
		},
		.args_schema = discard_schema(),
		.activate = [this] { new_level(); } });

	// Anyone but a person at the menu (an agent, a test, a game) names its files with check_agent_level_path and
	// never overwrites another file nor drops unsaved changes.
	const auto restricted = [](const cmd::call& request) { return request.from.kind != cmd::origin_kind::ui; };
	const auto file_state = [this](const std::string& relative) {
		return m_hooks.inspect_level ? m_hooks.inspect_level(relative) : inspect_level_file(m_hooks.resources_path, relative);
	};

	add({ .id = "file.open", .label = "Open Level...", .menu_path = "File",
		.execute_call = [this, level_change_busy, restricted, file_state](const cmd::call& request) {
			const json::value& args = request.args;
			if (auto busy = level_change_busy()) return *busy;
			if (!restricted(request)) {
				if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
				m_hooks.load_level(from_json_string(args.as_object().at("path").as_string()));
				return action_result::success(json::object{ { "level_loading", true } });
			}
			if (wants_discard(args)) {
				return action_result::failure(cmd::codes::invalid_args, "discard_unsaved is not available here: save the level first, a person drops unsaved changes in the editor");
			}
			const agent_level_path named = check_agent_level_path(args.as_object().at("path").as_string());
			if (!named.ok()) return action_result::failure(cmd::codes::invalid_args, named.error);
			switch (file_state(named.path)) {
			case level_file_state::missing: return action_result::failure(cmd::codes::not_found, "no such level: " + named.path);
			case level_file_state::unsafe: return action_result::failure(cmd::codes::invalid_args, "'" + named.path + "' is not a plain file inside the project");
			case level_file_state::file: break;
			}
			if (m_hooks.is_dirty()) return action_result::failure("needs_input", AGENT_UNSAVED_MESSAGE);
			if (!m_hooks.load_level_tag) return action_result::failure("failed", "opening a level by its path is not available");
			if (const std::optional<std::string> refused = m_hooks.load_level_tag(named.path)) {
				return action_result::failure(cmd::codes::invalid_args, *refused);
			}
			return action_result::success(json::object{ { "level_loading", true } });
		},
		.args_schema = path_schema(true, LEVEL_PATH_DESCRIPTION),
		.result_schema = json::object{
			{ "type", "object" },
			{ "properties", json::object{ { "level_loading", json::object{ { "type", "boolean" } } } } },
			{ "additionalProperties", false } },
		.activate = [this] { open_level(); },
		.category = cmd::agent_category::scene_edit });

	// The document does not belong to its file while a level is loading: a save then would write the old worlds over the
	// level being opened.
	const auto level_write_busy = [this]() -> std::optional<action_result> {
		if (!m_hooks.level_write_blocker) return std::nullopt;
		if (std::optional<std::string> why = m_hooks.level_write_blocker()) return action_result::failure(cmd::codes::busy, *why);
		return std::nullopt;
	};

	add({ .id = "file.save", .label = "Save Level", .menu_path = "File", .shortcut = "Ctrl+S",
		.execute = [this, level_write_busy](const json::value&) {
			if (auto busy = level_write_busy()) return *busy;
			if (!m_hooks.has_scene()) return action_result::failure("failed", "no level is loaded");
			if (m_hooks.level_path().empty()) return action_result::failure("failed", "the level has no path yet: use file.save_as");
			if (!m_hooks.quick_save()) return action_result::failure("failed", "the level could not be written");
			return action_result::success();
		},
		.activate = [this] { quick_save(); },
		.category = cmd::agent_category::scene_edit });

	add({ .id = "file.save_as", .label = "Save Level As...", .menu_path = "File", .shortcut = "Ctrl+Shift+S",
		.execute_call = [this, restricted, file_state, level_write_busy](const cmd::call& request) {
			if (auto busy = level_write_busy()) return *busy;
			if (!m_hooks.has_scene()) return action_result::failure("failed", "no level is loaded");
			std::string target(request.args.as_object().at("path").as_string());
			if (restricted(request)) {
				const agent_level_path named = check_agent_level_path(target);
				if (!named.ok()) return action_result::failure(cmd::codes::invalid_args, named.error);
				switch (file_state(named.path)) {
				case level_file_state::unsafe: return action_result::failure(cmd::codes::invalid_args, "'" + named.path + "' is not a plain file inside the project");
				case level_file_state::file:
					if (named.path != m_hooks.level_path()) {
						return action_result::failure("needs_input", "'" + named.path + "' exists and is not this level: choose a path that does not exist, or ask a person to overwrite it in the editor");
					}
					break;
				case level_file_state::missing: break;
				}
				target = named.path;
			}
			if (!m_hooks.save_level_as(target)) return action_result::failure("failed", "the level could not be written");
			return action_result::success();
		},
		.args_schema = path_schema(false, LEVEL_PATH_DESCRIPTION),
		.activate = [this] { save_level(); },
		.category = cmd::agent_category::scene_edit });

	const auto import_request_of = [](const json::object& args) {
		const auto text_of = [&args](const char* key) {
			const json::value* value = args.if_contains(key);
			return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string{};
		};
		return agent_import_request{ text_of("path"), text_of("name"), text_of("folder"), text_of("files_token") };
	};
	const auto import_available = [this] {
		return static_cast<bool>(m_hooks.import_model) || (m_hooks.agent_import != nullptr && m_hooks.agent_import->available());
	};
	constexpr const char* IMPORT_UNAVAILABLE = "Model import is not available: this editor was built without assimp (SNK_WITH_ASSIMP=OFF).";

	add({ .id = "file.import_model", .label = "Import Model...", .menu_path = "File", .separator_before = true,
		.disabled_tooltip = IMPORT_UNAVAILABLE,
		.disabled_reason = [this] {
			const std::string why = m_hooks.agent_import != nullptr ? m_hooks.agent_import->unavailable_reason() : std::string{};
			return why.empty() ? std::string(IMPORT_UNAVAILABLE) : why;
		},
		.execute_call = [this, import_request_of](const cmd::call& request) {
			const json::object& args = request.args.as_object();
			const bool dialog = request.from.kind == cmd::origin_kind::ui && !args.contains("name") && !args.contains("folder");
			if (dialog) {
				// A person at the menu or a script of theirs with only a path: the dialog flow, as before.
				if (!m_hooks.import_model) return action_result::failure("failed", "model import is not built in");
				m_hooks.import_model(from_json_string(args.at("path").as_string()));
				return action_result::success();
			}
			// An agent, or a person naming the asset and its folder: no dialog, one background job.
			if (m_hooks.agent_import == nullptr) return action_result::failure(cmd::codes::disabled, IMPORT_UNAVAILABLE);
			const agent_model_import::started started = m_hooks.agent_import->start(import_request_of(args), request.from, !request.confirmation.empty());
			if (!started.ok()) return action_result::failure(started.code, started.error);
			return action_result::success(json::object{ { "job", static_cast<std::int64_t>(started.job) } });
		},
		.args_schema = json::object{
			{ "type", "object" },
			{ "properties", json::object{
				{ "path", json::object{ { "type", "string" }, { "minLength", 1 },
					{ "description", "Absolute path of the model file (.glb, .gltf, .obj, .fbx); a regular file, not a link. An agent's import asks a person first." } } },
				{ "name", json::object{ { "type", "string" }, { "minLength", 1 },
					{ "description", "Asset name (default: the file name without extension). Giving it, or a folder, imports without the dialog." } } },
				{ "folder", json::object{ { "type", "string" }, { "minLength", 1 },
					{ "description", "Folder below res:// (default: assets/<name>). The import never overwrites an existing file." } } },
				{ "files_token", json::object{ { "type", "string" }, { "minLength", 1 },
					{ "description", "Only for the second call: when the job of the first call ends with needs_files_approval (the model refers to other files), call again with the same arguments and this token; a person is shown those files and decides." } } } } },
			{ "required", json::array{ "path" } },
			{ "additionalProperties", false } },
		.result_schema = json::object{
			{ "type", "object" },
			{ "properties", json::object{ { "job", json::object{ { "type", "integer" }, { "minimum", 0 },
				{ "description", "The import job: job.get shows its progress and, when done, {prefab, assets}, or {needs_files_approval, files_token, file_count} when the model refers to other files (nothing is written then); job.cancel stops it and takes back what it wrote." } } } } },
			{ "additionalProperties", false } },
		.activate = [this] { import_model(); },
		.enabled = import_available,
		.category = cmd::agent_category::scene_edit,
		.risk = cmd::danger::confirm,
		.describe = [this, import_request_of](const cmd::call& request) -> std::optional<std::string> {
			if (m_hooks.agent_import == nullptr) return std::nullopt;
			return m_hooks.agent_import->describe(import_request_of(request.args.as_object()), request.from);
		},
		.starts_job = true });

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
		.result_schema = switched_project_schema(),
		.activate = [this] { new_project(); } });

	add({ .id = "file.open_project", .label = "Open Project...", .menu_path = "File",
		.execute = [this](const json::value& args) {
			if (!m_hooks.open_project) return action_result::failure("failed", "projects are not available here");
			if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
			return switch_to_project(from_json_string(args.as_object().at("path").as_string()), flag(args, "allow_engine_version_mismatch"));
		},
		.args_schema = open_project_schema(),
		.result_schema = switched_project_schema(),
		.activate = [this] { open_project(); } });

	// One item per recent project: the list only changes when the editor restarts on another project.
	for (std::size_t i = 0; i < m_hooks.recent_projects.size(); ++i) {
		const std::filesystem::path manifest = m_hooks.recent_projects[i];
		const std::string id = std::format("file.recent_project.{}", i);
		add({ .id = id,
			.label = std::format("{}. {}  ({})", i + 1, to_json_string(manifest.parent_path().filename()),
				to_json_string(manifest.parent_path())),
			.menu_path = "File", .separator_before = i == 0,
			.execute = [this, manifest](const json::value& args) {
				if (m_hooks.is_dirty() && !wants_discard(args)) return action_result::failure("needs_input", UNSAVED_MESSAGE);
				return switch_to_project(manifest, false);
			},
			.args_schema = discard_schema(),
			.result_schema = switched_project_schema(),
			// file.open_project does the same for agents and scripts.
			.expose_as_tool = false,
			.activate = [this, id] {
				guard_unsaved([this, id](bool discard) {
					invoke_or_report(id, json::object{ { "discard_unsaved", discard } }, PROJECT_NOT_OPENED);
				});
			},
			.enabled = [manifest] {
				std::error_code ec;
				return std::filesystem::is_regular_file(manifest, ec);
			} });
	}

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
