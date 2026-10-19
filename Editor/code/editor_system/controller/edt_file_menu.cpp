#include "edt_file_menu.h"
#include "edt_dockspace.h"
#include <snk/common/logger/engine_log.h>
#include <utility>

namespace {
	constexpr const char* DEFAULT_LEVEL_PATH = "levels/my_level.desc";
}

edt::file_menu::file_menu(dialog_service& dialogs, file_menu_hooks hooks)
	: m_dialogs(dialogs)
	, m_hooks(std::move(hooks))
{
}

void edt::file_menu::guard_unsaved(std::function<void()> action)
{
	if (!m_hooks.is_dirty()) {
		action();
		return;
	}
	m_dialogs.confirm_unsaved([this, alive = std::weak_ptr<bool>(m_alive), action = std::move(action)](unsaved_choice choice) {
		if (alive.lock()) on_unsaved_choice(choice, action);
	});
}

void edt::file_menu::on_unsaved_choice(unsaved_choice choice, const std::function<void()>& action)
{
	switch (choice) {
	case unsaved_choice::save:
		if (!m_hooks.has_scene()) {
			egLOG("Editor/Save", "No scene loaded - nothing to save.");
			return;
		}
		if (!m_hooks.level_path().empty()) {
			m_hooks.quick_save();
			action();
			return;
		}
		// Untitled level: Save-As runs first, the action follows a successful write.
		ask_save_path(action);
		return;
	case unsaved_choice::discard:
		m_hooks.discard_changes();
		action();
		return;
	case unsaved_choice::cancel:
		return;
	}
}

void edt::file_menu::new_level()
{
	guard_unsaved([this] { m_hooks.new_level(); });
}

void edt::file_menu::exit()
{
	guard_unsaved([this] { m_hooks.exit(); });
}

void edt::file_menu::open_level()
{
	pick_file_options options;
	options.title = "Load Level";
	options.start_path = m_hooks.resources_path;
	options.extensions = { ".desc" };
	m_dialogs.pick_file(std::move(options), [this, alive = std::weak_ptr<bool>(m_alive)](const std::filesystem::path& picked) {
		if (alive.lock()) m_hooks.load_level(picked);
	});
}

void edt::file_menu::import_model()
{
	pick_file_options options;
	options.title = "Import Model";
	options.start_path = std::filesystem::current_path();
	options.extensions = { ".glb", ".obj", ".fbx", ".gltf" };
	m_dialogs.pick_file(std::move(options), [this, alive = std::weak_ptr<bool>(m_alive)](const std::filesystem::path& picked) {
		if (alive.lock()) m_hooks.import_model(picked);
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
	m_save_as_id = m_dialogs.save_as(std::move(initial), [this, alive = std::weak_ptr<bool>(m_alive)](const std::string& path) {
		if (!alive.lock()) return;
		auto after = std::move(m_after_save);
		m_after_save.clear();
		if (!m_hooks.save_level_as(path)) return;
		for (const auto& action : after) action();
	});
}

void edt::file_menu::quick_save()
{
	if (!m_hooks.has_scene()) return;
	if (!m_hooks.level_path().empty()) {
		m_hooks.quick_save();
	} else {
		save_level();
	}
}

void edt::file_menu::attach(dockspace& menu)
{
	menu.set_on_new_level([this] { new_level(); });
	menu.set_on_open_level([this] { open_level(); });
	menu.set_on_save_level([this] { save_level(); });
	menu.set_on_quick_save([this] { quick_save(); });
	menu.set_on_import_model([this] { import_model(); });
	menu.set_on_exit([this] { exit(); });
}
