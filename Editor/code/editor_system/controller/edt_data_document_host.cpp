#include "edt_data_document_host.h"
#include "edt_data_document.h"
#include "edt_data_document_panel.h"
#include <snk/common/logger/engine_log.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_panel_registry.h>
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
	auto panel = std::make_shared<data_document_panel>(data, m_services, id);
	panel->set_on_close([this, id] { request_close(id); });
	panel->set_on_save([this, id] { request_save(id); });
	panel->set_on_reload([this, id] { request_reload(id); });
	panel->set_on_overwrite([this, id] { request_overwrite(id); });
	registration registered = m_context.panels().add({
		.id = panel->get_id(), .title = panel->get_title(), .slot = dock_slot::floating, .open_by_default = true,
		.show_in_view_menu = false, .panel = panel });
	m_windows.emplace(id, window{ std::move(panel), &data, std::move(registered) });
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
		ask_conflict(id);
		return;
	}
	show_error(id, saved.error);
}

void edt::data_document_host::ask_conflict(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	m_context.dialogs().choose("File changed on disk",
		std::format("'{}' was changed on disk since you opened it.\n\nReload reads the file again and drops your unsaved changes. "
			"Overwrite replaces the file with your version.", found->second.data->title()),
		{ "Reload (drop my changes)", "Overwrite the file", "Cancel" },
		[this, id, alive = std::weak_ptr<int>(m_alive)](std::optional<std::size_t> choice) {
			if (!alive.lock() || !choice) return;
			if (*choice == 0) reload_now(id);
			else if (*choice == 1) overwrite_now(id);
		});
}

void edt::data_document_host::reload_now(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	if (const open_result reloaded = found->second.data->reload(); !reloaded.opened) {
		show_error(id, reloaded.error);
		return;
	}
	found->second.panel->set_error({});
}

void edt::data_document_host::overwrite_now(document_id id)
{
	const auto found = m_windows.find(id);
	if (found == m_windows.end()) return;
	if (const save_result saved = found->second.data->save_over(); !saved.saved) {
		show_error(id, saved.error);
		return;
	}
	found->second.panel->set_error({});
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
			if (alive.lock() && choice && *choice == 0) overwrite_now(id);
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
			egLOG_WARN("editor", "{}", saved.error);
			m_context.dialogs().message(message_level::error, std::format("'{}' was not saved and stays open:\n{}", open->title(), saved.error));
			return false;
		}
	}
	return true;
}
