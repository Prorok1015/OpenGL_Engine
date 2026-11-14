#include "edt_data_document_host.h"
#include "edt_data_document.h"
#include "edt_data_document_panel.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_panel_registry.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <format>
#include <optional>

edt::data_document_host::data_document_host(editor_context& context, editor_services& services,
	std::function<desc_file_layout()> layout, std::function<void(document&)> focus_other)
	: m_context(context)
	, m_services(services)
	, m_layout(std::move(layout))
	, m_focus_other(std::move(focus_other))
{
	m_closed = m_context.events().document_closed.subscribe([this](document_id id) { m_windows.erase(id); });
}

edt::data_document_host::~data_document_host()
{
	shutdown();
}

void edt::data_document_host::shutdown()
{
	m_closed.disconnect();
	m_windows.clear();
}

void edt::data_document_host::focus(document& open)
{
	m_context.documents().set_active(open.id());
	if (const auto found = m_windows.find(open.id()); found != m_windows.end()) {
		found->second.panel->set_open(true);
		found->second.panel->request_focus();
		return;
	}
	if (m_focus_other) m_focus_other(open);
}

void edt::data_document_host::commit_pending_except(std::string_view focused_window)
{
	for (auto& entry : m_windows) {
		if (entry.second.panel->get_title() != focused_window) entry.second.data->commit_pending();
	}
}

void edt::data_document_host::follow_focus()
{
	const ImGuiContext* imgui = ImGui::GetCurrentContext();
	if (imgui == nullptr) return;
	const ImGuiWindow* focused = imgui->NavWindow != nullptr ? imgui->NavWindow->RootWindow : nullptr;
	const unsigned int id = focused != nullptr ? focused->ID : 0u;
	if (id == m_focused_window) return;
	m_focused_window = id;
	if (focused == nullptr) return;

	// An edit in the middle of a drag is recorded before another window takes over: it must not be folded into an edit made there.
	const std::string_view name = focused->Name;
	commit_pending_except(name);

	// The main menu bar lives in a window of the dockspace host: a click on a menu moves the focus there, and closing the menu
	// leaves it there. None of these is a window the person works in, so the document they came from stays active.
	constexpr ImGuiWindowFlags service = ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_Popup | ImGuiWindowFlags_Tooltip
		| ImGuiWindowFlags_ChildMenu | ImGuiWindowFlags_Modal;
	if ((focused->Flags & service) != 0 || name.starts_with("##")) return;

	document_manager& manager = m_context.documents();
	const auto data_window = std::find_if(m_windows.begin(), m_windows.end(),
		[name](const auto& entry) { return entry.second.panel->get_title() == name; });
	if (data_window != m_windows.end()) {
		manager.set_active(data_window->first);
		return;
	}
	const std::vector<panel_info> panels = m_context.panels().panels();
	const auto taken = std::find_if(panels.begin(), panels.end(), [name](const panel_info& panel) { return panel.title == name; });
	if (taken != panels.end() && taken->id == "prefab_editor" && manager.preview() != nullptr) {
		manager.set_active(manager.preview()->id());
		return;
	}
	manager.set_active(manager.scene().id());
}

void edt::data_document_host::open(const res::tag& tag, bool quiet)
{
	document_manager& documents = m_context.documents();
	if (document* open = documents.find_by_source(tag)) {
		focus(*open);
		return;
	}
	auto created = std::make_unique<data_document>(m_services, m_layout());
	if (const open_result opened = created->open(tag); !opened.opened) {
		if (quiet || opened.unsuitable) {
			egLOG_INFO("editor", "'{}' is not opened as a data document: {}", tag.view(), opened.error);
			return;
		}
		egLOG_WARN("editor", "{}", opened.error);
		m_context.dialogs().message(message_level::warning, std::format("'{}' cannot be opened as a data document:\n{}", tag.view(), opened.error));
		return;
	}
	data_document& data = *created;
	const document_id id = documents.adopt(std::move(created)).id();
	if (!quiet) documents.set_active(id);
	auto panel = std::make_shared<data_document_panel>(data, m_services, id, quiet);
	panel->set_on_close([this, id] { request_close(id); });
	panel->set_on_save([this, id] { request_save(id); });
	panel->set_has_project_migration([this] { return m_context.actions().contains("project.migrate"); });
	panel->set_asset_source(m_asset_source);
	panel->set_on_reload([this, id] { request_reload(id); });
	panel->set_on_overwrite([this, id] { request_overwrite(id); });
	registration registered = m_context.panels().add({
		.id = panel->get_id(), .title = panel->get_title(), .slot = dock_slot::floating, .open_by_default = true,
		.show_in_view_menu = false, .panel = panel });
	m_windows.emplace(id, window{ std::move(panel), &data, std::move(registered) });
}

void edt::data_document_host::set_asset_source(std::function<const picker::catalog*()> source)
{
	m_asset_source = std::move(source);
	for (auto& [id, open] : m_windows) {
		open.panel->set_asset_source(m_asset_source);
	}
}

void edt::data_document_host::request_close(document_id id)
{
	document* open = m_context.documents().find(id);
	const auto found = m_windows.find(id);
	if (!open || found == m_windows.end()) return;
	if (!open->is_dirty()) {
		close(id);
		return;
	}
	// The window stays while the question is open (closing it asked, nothing was decided).
	found->second.panel->set_open(true);
	m_context.dialogs().confirm_unsaved([this, id, alive = std::weak_ptr<int>(m_alive)](unsaved_choice choice) {
		if (!alive.lock()) return;
		document* still_open = m_context.documents().find(id);
		if (!still_open || choice == unsaved_choice::cancel) return;
		if (choice == unsaved_choice::save) {
			if (const save_result saved = still_open->save(); !saved.saved) {
				if (in_conflict(id)) {
					ask_conflict(id, [this, id](bool resolved) {
						if (resolved) close(id);
					});
					return;
				}
				if (const auto window = m_windows.find(id); window != m_windows.end() && window->second.data->needs_version_confirmation()) {
					ask_version(id, false, [this, id](bool saved_now) {
						if (saved_now) close(id);
					});
					return;
				}
				egLOG_WARN("editor", "{}", saved.error);
				m_context.dialogs().message(message_level::error, std::format("'{}' was not saved and stays open:\n{}", still_open->title(), saved.error));
				return;
			}
		}
		close(id);
	});
}

void edt::data_document_host::show_error(document_id id, const std::string& text)
{
	egLOG_WARN("editor", "{}", text);
	if (const auto found = m_windows.find(id); found != m_windows.end()) found->second.panel->set_error(text);
}

bool edt::data_document_host::in_conflict(document_id id) const
{
	const auto found = m_windows.find(id);
	return found != m_windows.end() && found->second.data->changed_on_disk();
}

void edt::data_document_host::request_save(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	const save_result saved = found->second.data->save();
	if (saved.saved) {
		found->second.panel->set_error({});
		return;
	}
	if (found->second.data->changed_on_disk()) {
		ask_conflict(id, {});
		return;
	}
	if (found->second.data->needs_version_confirmation()) {
		ask_version(id, false, {});
		return;
	}
	show_error(id, saved.error);
}

void edt::data_document_host::ask_version(document_id id, bool over, std::function<void(bool saved)> done)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	const data_document& data = *found->second.data;
	const bool root_outdated = data.file_version() < data.current_version();
	const std::string target = root_outdated ? std::format("version {}", data.current_version()) : std::string{ "the current versions" };
	m_context.dialogs().choose(std::format("Save as {}", target),
		root_outdated
			? std::format("'{}' is of version {}; saving writes {}. An older editor or branch may not read the new file.",
				data.title(), data.file_version(), target)
			: std::format("'{}' holds objects of older versions; saving writes {}. An older editor or branch may not read the new file.",
				data.title(), target),
		{ std::format("Save {}", target), "Cancel" },
		[this, id, over, done = std::move(done), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock()) return;
			bool saved = false;
			if (choice && *choice == 0) {
				const auto window = m_windows.find(id);
				if (window != m_windows.end()) {
					data_document& target_document = *window->second.data;
					target_document.confirm_version_write();
					const save_result result = over ? target_document.save_over() : target_document.save();
					if (result.saved) {
						window->second.panel->set_error({});
						saved = true;
					} else if (in_conflict(id)) {
						ask_conflict(id, done);
						return;
					} else {
						show_error(id, result.error);
					}
				}
			}
			if (done) done(saved);
		});
}

void edt::data_document_host::ask_conflict(document_id id, std::function<void(bool resolved)> done)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	m_context.dialogs().choose("File changed on disk",
		std::format("'{}' was changed on disk since you opened it.\n\nReload reads the file again and drops your unsaved changes. "
			"Overwrite replaces the file with your version.", found->second.data->title()),
		{ "Reload (drop my changes)", "Overwrite the file", "Cancel" },
		[this, id, done = std::move(done), alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock()) return;
			if (choice && *choice == 1) {
				overwrite_now(id, done);
				return;
			}
			const bool resolved = choice && *choice == 0 && reload_now(id);
			if (done) done(resolved);
		});
}

bool edt::data_document_host::reload_now(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return false;
	if (const open_result reloaded = found->second.data->reload(); !reloaded.opened) {
		show_error(id, reloaded.error);
		return false;
	}
	found->second.panel->set_error({});
	return true;
}

void edt::data_document_host::overwrite_now(document_id id, std::function<void(bool resolved)> done)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) {
		if (done) done(false);
		return;
	}
	if (const save_result saved = found->second.data->save_over(); !saved.saved) {
		if (found->second.data->needs_version_confirmation()) {
			// The answer to that question settles `done`.
			ask_version(id, true, std::move(done));
			return;
		}
		show_error(id, saved.error);
		if (done) done(false);
		return;
	}
	found->second.panel->set_error({});
	if (done) done(true);
}

void edt::data_document_host::request_reload(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	if (!found->second.data->is_dirty()) {
		reload_now(id);
		return;
	}
	m_context.dialogs().choose("Reload from disk",
		std::format("Drop your unsaved changes to '{}' and read the file again?", found->second.data->title()),
		{ "Reload (drop my changes)", "Cancel" },
		[this, id, alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (alive.lock() && choice && *choice == 0) reload_now(id);
		});
}

void edt::data_document_host::request_overwrite(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	m_context.dialogs().choose("Overwrite the file",
		std::format("Replace the file '{}' on disk with your version? What was written there is lost.", found->second.data->title()),
		{ "Overwrite", "Cancel" },
		[this, id, alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (alive.lock() && choice && *choice == 0) overwrite_now(id, {});
		});
}

void edt::data_document_host::close(document_id id)
{
	m_windows.erase(id);
	m_context.documents().close(id);
}

std::vector<edt::document_id> edt::data_document_host::dirty() const
{
	std::vector<document_id> out;
	for (const auto& [id, shown] : m_windows) {
		if (const document* open = m_context.documents().find(id); open && open->is_dirty()) out.push_back(id);
	}
	// The ids grow in the order the windows were opened: what is saved first does not depend on the hash table.
	std::ranges::sort(out);
	return out;
}

bool edt::data_document_host::save_all()
{
	for (const document_id id : dirty()) {
		document* open = m_context.documents().find(id);
		if (!open) continue;
		if (const save_result saved = open->save(); !saved.saved) {
			if (in_conflict(id)) {
				ask_conflict(id, {});
				return false;
			}
			if (const auto window = m_windows.find(id); window != m_windows.end() && window->second.data->needs_version_confirmation()) {
				ask_version(id, false, {});
				return false;
			}
			egLOG_WARN("editor", "{}", saved.error);
			m_context.dialogs().message(message_level::error, std::format("'{}' was not saved and stays open:\n{}", open->title(), saved.error));
			return false;
		}
	}
	return true;
}
