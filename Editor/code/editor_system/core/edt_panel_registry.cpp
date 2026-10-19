#include <snk/editor/edt_panel_registry.h>
#include "edt_panel_base.h"
#include <snk/common/logger/engine_log.h>
#include <algorithm>
#include <utility>

struct edt::panel_registry::entry
{
	std::uint64_t key = 0;
	panel_desc desc;
};

edt::panel_registry::panel_registry() = default;
edt::panel_registry::~panel_registry() = default;

edt::registration edt::panel_registry::add(panel_desc desc)
{
	if (desc.id.empty() || !desc.panel) {
		egLOG_ERROR("Editor/Panels", "panel '{}' not registered: it needs an id and a panel", desc.id);
		return {};
	}
	if (desc.panel->get_id() != desc.id || desc.panel->get_title() != desc.title) {
		egLOG_ERROR("Editor/Panels", "panel '{}' not registered: id/title '{}'/'{}' differ from the panel's own '{}'/'{}'",
			desc.id, desc.id, desc.title, desc.panel->get_id(), desc.panel->get_title());
		return {};
	}
	if (contains(desc.id)) {
		egLOG_ERROR("Editor/Panels", "panel '{}' not registered: the id is already taken", desc.id);
		return {};
	}

	desc.panel->set_open(desc.open_by_default);
	auto added = std::make_shared<entry>();
	added->key = m_next_key++;
	added->desc = std::move(desc);
	const std::uint64_t key = added->key;
	m_entries.push_back(std::move(added));
	return registration(m_alive, [this, key] { remove(key); });
}

void edt::panel_registry::remove(std::uint64_t key)
{
	std::erase_if(m_entries, [key](const std::shared_ptr<entry>& e) { return e->key == key; });
}

bool edt::panel_registry::contains(std::string_view id) const
{
	return find(id) != nullptr;
}

edt::panel_base* edt::panel_registry::find(std::string_view id) const
{
	const auto it = std::find_if(m_entries.begin(), m_entries.end(),
		[id](const std::shared_ptr<entry>& e) { return e->desc.id == id; });
	return it != m_entries.end() ? (*it)->desc.panel.get() : nullptr;
}

bool edt::panel_registry::is_open(std::string_view id) const
{
	const panel_base* panel = find(id);
	return panel && panel->is_open();
}

bool edt::panel_registry::set_open(std::string_view id, bool open)
{
	panel_base* panel = find(id);
	if (!panel) return false;
	panel->set_open(open);
	return true;
}

std::vector<edt::panel_info> edt::panel_registry::panels() const
{
	std::vector<panel_info> out;
	out.reserve(m_entries.size());
	for (const auto& e : m_entries) {
		out.push_back({ e->desc.id, e->desc.title, e->desc.slot, e->desc.panel->is_open(), e->desc.show_in_view_menu });
	}
	return out;
}

void edt::panel_registry::render_all()
{
	// By index: a panel may add or remove registrations while it draws.
	for (std::size_t i = 0; i < m_entries.size(); ++i) {
		const std::shared_ptr<panel_base> panel = m_entries[i]->desc.panel;
		panel->render();
	}
}
