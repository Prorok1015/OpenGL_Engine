#include "edt_file_menu_component.h"
#include "edt_asset_component.h"
#include "edt_asset_open_component.h"
#include "edt_asset_problems.h"
#include "edt_edit_menu.h"
#include "edt_file_menu.h"
#include "edt_import_component.h"
#include "edt_level_controller.h"
#include "edt_level_disk_banner.h"
#include "edt_level_load_component.h"
#include "edt_main_menu.h"
#include "edt_path_text.h"
#include "edt_prefab_document.h"
#include "edt_prefab_editor_component.h"
#include "edt_problems_component.h"
#include "edt_recent_projects.h"
#include "edt_save_all.h"
#include "edt_scene_document.h"
#include "edt_template_ids.h"
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/platform/plt_process.h>
#include <snk/project/prj_create.h>
#include <snk/resource/res_system.h>
#include <snk/window/wnd_window_system.h>
#include <snk/gui/gui_theme.h>
#include <imgui.h>
#include <utility>

// Editor/data (editor_module): the project templates live under it.
CFG_VAR_EXT_PATH(cfg_edt_data_path);

namespace
{
	// Switching projects restarts the editor (EPIC-42 #158): the same program and config file, the other project.
	// Overrides given with --set are not carried over.
	// --open-migration (#761): the new editor opens the Project Migration window once the project is open.
	std::optional<std::string> start_editor_on(const std::filesystem::path& manifest_file, bool allow_engine_version_mismatch,
		bool open_migration)
	{
		const std::filesystem::path program = platform::current_executable();
		if (program.empty()) {
			return std::string{ "the editor cannot find its own program file" };
		}
		std::vector<std::string> args;
		if (const auto config = cfg::loaded_config_path(); !config.empty()) {
			args.insert(args.end(), { "--config", edt::utf8_of(config) });
		}
		args.insert(args.end(), { "--project", edt::utf8_of(manifest_file) });
		if (allow_engine_version_mismatch) {
			args.push_back("--allow-engine-version-mismatch");
		}
		if (open_migration) {
			args.push_back("--open-migration");
		}
		egLOG_INFO("Editor/Project", "restarting the editor on '{}'", edt::utf8_of(manifest_file));
		auto started = platform::spawn_detached(program, args);
		if (!started) {
			egLOG_ERROR("Editor/Project", "the editor was not restarted on '{}': {}", edt::utf8_of(manifest_file), started.error);
			return started.error;
		}
		return std::nullopt;
	}


	// What the level file at a project path is in the layers of res:// (a lower content root, the engine), not only the top
	// one: an agent's save_as onto a file that only a lower layer has would shadow it, and its open of such a level works.
	// A link or a folder in any layer is unsafe; a layer that is not a folder (a RAM or archive resolver) is asked itself.
	edt::level_file_state inspect_level_in_layers(const res::resource_system& resources, const std::string& relative)
	{
		bool found = false;
		for (const std::filesystem::path& root : resources.get_resource_layers()) {
			switch (edt::inspect_level_file(root, relative)) {
			case edt::level_file_state::unsafe: return edt::level_file_state::unsafe;
			case edt::level_file_state::file: found = true; break;
			case edt::level_file_state::missing: break;
			}
		}
		if (found) return edt::level_file_state::file;
		if (const auto raw = resources.fetch_data(res::tag::make(relative))) {
			raw->wait();
			if (raw->is_ready()) return edt::level_file_state::file;
		}
		return edt::level_file_state::missing;
	}
}

edt::file_menu_component::file_menu_component(ports from)
	: m_ports(std::move(from))
{
	m_tabs = std::make_unique<document_tabs>(m_ports.context.documents(),
		document_tab_ports{ focus_ports(), [this](document_id id) { m_ports.asset_open.data_documents().request_close(id); },
			[this](document_id id) { m_ports.prefab.request_close(id); }, [this] { return m_menu && m_menu->save_all_running(); },
			[this](document_id id) { if (m_ports.request_close_mesh) m_ports.request_close_mesh(id); },
			[this](document_id id) { if (m_ports.request_close_shader_source) m_ports.request_close_shader_source(id); } });
	file_menu_hooks hooks;
	hooks.has_scene = [this] { return m_ports.state.editor_tag.is_valid(); };
	hooks.is_dirty = [this] { return m_ports.state.is_dirty(); };
	hooks.level_path = [this] {
		return m_ports.state.level_tag.is_valid() ? std::string(m_ports.state.level_tag.relative()) : std::string{};
	};
	hooks.new_level = [this] {
		if (m_ports.stop_play) m_ports.stop_play();
		m_ports.level_load.cancel_settle();
		m_ports.problems.desc().clear(m_ports.context.problems(), "level");
		m_ports.level_ctrl.new_level();
	};
	hooks.quick_save = [this] { return m_ports.state.save().saved; };
	hooks.save_level_as = [this](const std::string& rel_path) {
		if (rel_path.find("://") != std::string::npos) {
			egLOG_ERROR("Editor/Save", "Level not saved: '{}' must be a path inside the project, without a protocol", rel_path);
			return false;
		}
		return m_ports.state.save_as(res::tag::make(rel_path)).saved;
	};
	hooks.save_focused_document = [this] { return m_ports.save_focused_shader && m_ports.save_focused_shader(); };
	hooks.discard_changes = [this] { m_ports.state.mark_clean(); };
	hooks.other_documents_dirty = [this] {
		return !m_ports.asset_open.data_documents().dirty().empty() || m_ports.prefab.dirty() != nullptr || (m_ports.meshes_dirty && m_ports.meshes_dirty())
			|| (m_ports.shaders_dirty && m_ports.shaders_dirty());
	};
	hooks.save_other_documents = [this] {
		if (!m_ports.prefab.apply_dirty()) return false;
		if (m_ports.save_meshes && !m_ports.save_meshes()) return false;
		if (m_ports.save_shaders && !m_ports.save_shaders()) return false;
		return m_ports.asset_open.data_documents().save_all();
	};
	hooks.discard_other_documents = [this] {
		for (const document_id id : m_ports.asset_open.data_documents().dirty()) m_ports.asset_open.data_documents().close(id);
		m_ports.prefab.close_dirty();
		if (m_ports.discard_meshes) m_ports.discard_meshes();
		if (m_ports.discard_shaders) m_ports.discard_shaders();
	};
	hooks.load_level = [this](const std::filesystem::path& abs_path) {
		const auto relative = abs_path.lexically_relative(m_ports.resources.get_resources_path());
		m_ports.level_load.begin_and_watch(res::tag::make(relative.string()));
	};
	// The commands an agent calls name a level by its project path: no file system path in between.
	hooks.load_level_tag = [this](const std::string& relative) -> std::optional<std::string> {
		if (!m_ports.level_load.begin_and_watch(res::tag::make(relative))) return "'" + relative + "' is not a level file";
		return std::nullopt;
	};
	hooks.inspect_level = [this](const std::string& relative) { return inspect_level_in_layers(m_ports.resources, relative); };
	const std::function<std::optional<std::string>()> level_write_blocker = [this]() -> std::optional<std::string> {
		if (auto migrating = migration_blocker()) return migrating;
		if (m_ports.level_load.in_progress()) return std::string{ "a level is loading" };
		// Whatever lifted a migration's block: a detached level has no worlds, so a save would write an empty level (#761).
		std::optional<std::string> stopped = m_ports.migration_stopped_reason ? m_ports.migration_stopped_reason() : std::nullopt;
		if (m_ports.level_ctrl.is_detached()) return stopped ? *stopped : std::string{ "the level is detached: it is not loaded from disk again yet" };
		return stopped;
	};
	hooks.level_write_blocker = level_write_blocker;
	m_ports.level_ctrl.write_blocker = level_write_blocker;
	// #1188: the file of the level changed on disk; read again at each save, so a missed signal is found too.
	const std::function<std::optional<std::string>()> level_disk_conflict = [this] { return m_ports.level_ctrl.disk_conflict(); };
	hooks.level_disk_conflict = level_disk_conflict;
	if (m_ports.level_disk) {
		hooks.ask_level_disk_conflict = [this](std::function<void(bool resolved)> resolved) {
			m_ports.level_disk->ask_save(std::move(resolved));
		};
	}
	// Save All (#1183): every document that can be written, then the questions about the ones that cannot (controller/edt_save_all.h).
	hooks.save_all = [this, level_write_blocker, level_disk_conflict] {
		save_all_scene_ports scene;
		scene.write_blocker = level_write_blocker;
		scene.level_path = [this] { return m_ports.state.level_tag.is_valid() ? std::string(m_ports.state.level_tag.relative()) : std::string{}; };
		scene.quick_save = [this] { return m_ports.state.save().saved; };
		scene.disk_conflict = level_disk_conflict;
		return save_all_write(m_ports.context.documents(), scene);
	};
	hooks.ask_unsaved = [this](const save_all_ask& ask, std::function<void(bool resolved)> resolved) {
		if (ask.item.kind == "data") {
			m_ports.asset_open.data_documents().resolve_unsaved(ask.item.id, std::move(resolved));
		} else if (ask.item.kind == "prefab") {
			m_ports.prefab.resolve_unsaved(ask, std::move(resolved));
		} else if (ask.item.kind == "scene" && ask.question == save_all_question::disk_changed && m_ports.level_disk) {
			m_ports.level_disk->ask_save(std::move(resolved));
		} else if (resolved) {
			resolved(false);
		}
	};
	// An agent's file.open / file.new would replace the start level behind the loading screen, and Play would start on
	// its level (#518). A person has no menu or shortcut then.
	hooks.level_change_blocker = [this]() -> std::optional<std::string> {
		if (auto migrating = migration_blocker()) return migrating;
		if (m_ports.startup_pending && m_ports.startup_pending()) return std::string{ "the start level is loading" };
		if (m_ports.level_load.in_progress()) return std::string{ "a level is loading" };
		return std::nullopt;
	};
#if SNK_HAS_ASSIMP
	hooks.import_model = [this](const std::filesystem::path& abs_path, const model_import_settings& settings) {
		m_ports.import.start_import(abs_path, settings);
	};
	hooks.ask_model_settings = [this](const std::filesystem::path& abs_path,
		std::function<void(const model_import_settings&)> import) {
		m_ports.import.ask_settings(abs_path, [import = std::move(import)](const std::filesystem::path&,
			const model_import_settings& settings) { import(settings); });
	};
#endif
	hooks.reimport_model = [this](const res::tag& prefab, const std::filesystem::path& source,
		const boost::json::value& settings) { return m_ports.import.reimport(prefab, source, settings); };
	hooks.ask_model_reimport_settings = [this](const res::tag& prefab, const std::filesystem::path& source,
		std::function<void(const model_import_settings&)> import) {
		m_ports.import.ask_reimport_settings(prefab, source, [import = std::move(import)](const std::filesystem::path&,
			const model_import_settings& settings) { import(settings); });
	};
	hooks.agent_import = m_ports.import.agent_import();
	hooks.image_import = m_ports.import.image_import();
	hooks.exit = m_ports.exit;
	hooks.exit_blocker = [this] { return migration_blocker(); };
	hooks.resources_path = m_ports.resources.get_resources_path();
	hooks.open_project = start_editor_on;
	// From disk, not through res::: a recursive copy into a folder that no mount covers yet.
	hooks.create_project = [](const std::filesystem::path& folder, const std::string& name) {
		// The template ships without .resource sidecars: each new project gets ids of its own (#765).
		return prj::create_project(*cfg_edt_data_path / "templates" / "projects" / "empty", folder, name,
			[](const std::filesystem::path& staging) { return give_template_fresh_ids(staging); });
	};
	// Next to the open project, else where the editor was started.
	hooks.projects_path = m_ports.projects_path;
	hooks.recent_projects = recent_projects_besides(m_ports.resources, m_ports.project_manifest);
	m_menu = std::make_unique<file_menu>(m_ports.context.dialogs(), m_ports.context.actions(), std::move(hooks));
	m_ports.level_ctrl.on_file_upgraded = [this](const std::string& text) { m_save_notice.show(text, timed_notice::clock::now()); };
}

edt::file_menu_component::~file_menu_component()
{
	// The level controller is declared before this component and outlives it: it must not call into a notice that is gone.
	m_ports.level_ctrl.on_file_upgraded = nullptr;
	m_ports.level_ctrl.write_blocker = nullptr;
}

std::optional<std::string> edt::file_menu_component::migration_blocker() const
{
	return m_ports.migration_blocker ? m_ports.migration_blocker() : std::nullopt;
}

void edt::file_menu_component::install(std::vector<registration>& out)
{
	for (auto& menu : register_builtin_menus(m_ports.context.actions())) out.push_back(std::move(menu));
	m_menu->register_actions();
}

void edt::file_menu_component::install_edit_actions(std::vector<registration>& out)
{
	const auto asset_undo_label = [this]() -> std::optional<std::string> { return m_ports.assets && m_ports.assets() ? m_ports.assets()->next_undo_label() : std::nullopt; };
	const auto asset_undo = [this] {
		if (!m_ports.assets || !m_ports.assets()) {
			return false;
		}
		const asset_fix_result result = m_ports.assets()->undo_last();
		if (!result.ok) {
			egLOG_ERROR("assets", "{}", result.error);
		}
		return result.ok;
	};
	for (auto& action : register_edit_actions(m_ports.context.actions(), m_ports.context.documents(), m_ports.context.dialogs(), [] { return ImGui::IsAnyItemActive(); },
			asset_undo_label, asset_undo, [this]() -> document* { return m_ports.prefab.inspector_prefab(); })) {
		out.push_back(std::move(action));
	}
}

void edt::file_menu_component::reimport_model(const res::tag& prefab)
{
	if (m_menu) m_menu->reimport_model(prefab);
}

void edt::file_menu_component::attach_window_close(wnd::window_system& windows)
{
	m_menu->attach_window_close(windows);
}

edt::document_focus_ports edt::file_menu_component::focus_ports()
{
	return { [this](const res::tag& source) { m_ports.asset_open.data_documents().open(source, false); },
		[this] { m_ports.prefab.show(); },
		[this](document_id id) { if (m_ports.show_mesh) m_ports.show_mesh(id); },
		[this](document_id id) { if (m_ports.show_shader_source) m_ports.show_shader_source(id); } };
}

void edt::file_menu_component::focus_unsaved(document_id id)
{
	focus_document(m_ports.context.documents(), id, focus_ports());
}

void edt::file_menu_component::draw_document_tabs()
{
	if (!m_tabs) return;
	const document_tabs_bar::actions on{ [this](document_id id) { m_tabs->activate(id); }, [this](document_id id) { m_tabs->close(id); } };
	m_tabs_bar.draw(m_tabs->tabs(), m_ports.context.colors(), on);
}

void edt::file_menu_component::draw_unsaved_list(const std::vector<unsaved_group>& groups)
{
	const gui::theme_semantic_colors& colors = m_ports.context.colors();
	for (const unsaved_group& group : groups) {
		ImGui::TextColored(colors.text_secondary, "%s", group.heading.c_str());
		ImGui::Indent();
		for (const unsaved_row& row : group.rows) {
			ImGui::PushID(static_cast<int>(row.id));
			if (ImGui::Selectable(row.title.c_str())) focus_unsaved(row.id);
			ImGui::PopID();
		}
		ImGui::Unindent();
	}
}

void edt::file_menu_component::draw_unsaved_indicator()
{
	constexpr const char* popup_id = "unsaved_documents";
	document_manager& documents = m_ports.context.documents();
	const std::size_t count = count_unsaved(documents);
	if (count == 0) return;
	ImGui::TextColored(m_ports.context.colors().warning, "Unsaved (%zu)", count);
	if (ImGui::IsItemClicked()) ImGui::OpenPopup(popup_id);
	if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen(popup_id)) {
		const std::string text = unsaved_text(list_unsaved(documents)) + "\n\nClick for the list. File > Save All (" + std::string(k_save_all_shortcut) + ") writes them.";
		ImGui::SetTooltip("%s", text.c_str());
	}
	if (ImGui::BeginPopup(popup_id)) {
		draw_unsaved_list(unsaved_groups(list_unsaved(documents)));
		ImGui::Separator();
		if (ImGui::Button("Save All")) {
			ImGui::CloseCurrentPopup();
			m_menu->save_all();
		}
		ImGui::EndPopup();
	}
}

void edt::file_menu_component::draw_status()
{
	const std::optional<std::string_view> text = m_save_notice.text(timed_notice::clock::now());
	if (!text) return;
	ImGui::TextColored(m_ports.context.colors().warning, "%.*s", static_cast<int>(text->size()), text->data());
}
