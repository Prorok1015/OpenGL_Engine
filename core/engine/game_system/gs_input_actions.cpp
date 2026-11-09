#include <snk/game/gs_input_actions.h>
#include <snk/input/inp_actions.h>
#include <snk/project/prj_project.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log.h>

gs::input_actions::input_actions(res::resource_system& resources)
	: m_resources(resources)
{
}

gs::input_actions::~input_actions()
{
	drop();
}

void gs::input_actions::drop()
{
	if (!m_tag.empty()) {
		m_resources.unwatch(res::tag{ m_tag }, this);
	}
	m_tag.clear();
	m_handle = {};
	m_map.reset();
	m_changed = false;
	m_built = false;
	m_reported = false;
}

void gs::input_actions::update(const ds::app_data_storage& data)
{
	const bool open = data.has_value<prj::project_context>();
	update(open ? std::string_view{ data.require<prj::project_context>().data.input_actions } : std::string_view{});
}

void gs::input_actions::update(std::string_view tag)
{
	if (tag != m_tag) {
		drop();
		if (tag.empty()) return;
		m_tag = std::string{ tag };
		m_handle = m_resources.require<scn::input_actions_desc>(res::tag{ m_tag });
		m_resources.watch(res::tag{ m_tag }, this, [this](const res::tag&) { m_changed = true; });
	}
	if (m_tag.empty()) return;

	if (m_changed) {
		m_changed = false;
		m_handle = m_resources.require<scn::input_actions_desc>(res::tag{ m_tag });
		m_built = false;
		m_reported = false;
	}
	if (m_built) return;

	if (m_handle.is_ready()) {
		m_map = std::make_shared<const inp::action_map>(scn::build_action_map(*m_handle.get(), m_tag));
		m_built = true;
		egLOG_INFO("input", "game actions from {}: {}", m_tag, m_map->size());
	} else if (m_handle.has_error() && !m_reported) {
		m_reported = true;
		egLOG_ERROR("input", "game actions {} did not load{}: {}", m_tag, m_map ? ", the previous ones stay" : "",
			m_handle.error_text());
	}
}
