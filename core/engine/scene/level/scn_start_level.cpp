#include <snk/scene/level/scn_start_level.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/resource/res_diagnostics.h>
#include <snk/project/prj_project.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log.h>
#include <string>

std::string scn::start_level_name(const ds::app_data_storage& data, std::string_view override_level)
{
	if (!override_level.empty()) {
		return std::string{ override_level };
	}
	if (data.has_value<prj::project_context>()) {
		return data.require<prj::project_context>().data.start_scene;
	}
	return {};
}

res::tag scn::start_level_tag(std::string_view name)
{
	return name.find("://") != std::string_view::npos ? res::tag{ name } : res::tag::make(name);
}

std::optional<res::tag> scn::resolve_start_level(const ds::app_data_storage& data, std::string_view override_level)
{
	const std::string name = start_level_name(data, override_level);
	if (name.empty()) {
		return std::nullopt;
	}
	return start_level_tag(name);
}

void scn::report_level_loaded(const res::tag& level)
{
	egLOG_INFO("scene/level", "level loaded: {}", level.view());
}

void scn::report_level_failed(const res::tag& level, const res::res_handle<scn::level_desc>& handle)
{
	const std::string why = handle.error_text();
	egLOG_ERROR("scene/level", "failed to load level '{}'{}{}", level.view(), why.empty() ? "" : ": ", why);
	res::diagnostics_collector collector;
	handle.collect_into(collector);
	for (const auto& item : collector.items()) {
		const std::string where = item.json_path.empty() ? item.resource : item.resource + " " + item.json_path;
		if (item.severity == res::diagnostic_severity::error) {
			egLOG_ERROR("scene/level", "  {}: {}", where, item.reason);
		} else {
			egLOG_WARN("scene/level", "  {}: {}", where, item.reason);
		}
	}
}

scn::start_level_loader::start_level_loader(context_manager& contexts, res::resource_system& base_resources)
	: m_contexts(contexts)
	, m_base_resources(base_resources)
{
}

void scn::start_level_loader::begin(const res::tag& level)
{
	// The set "level" is built from the game layer's cache, so the desc is loaded there and load_level finds it.
	auto& resources = m_contexts.game_layer() ? m_contexts.game_layer()->resources() : m_base_resources;
	m_level = level;
	m_handle = resources.require<scn::level_desc>(level);
	m_state = state::loading;
}

scn::start_level_loader::state scn::start_level_loader::poll()
{
	if (m_state != state::loading) {
		return m_state;
	}
	if (m_handle.has_error()) {
		report_level_failed(m_level, m_handle);
		m_state = state::failed;
	} else if (m_handle.is_ready()) {
		if (m_contexts.load_level(m_level)) {
			report_level_loaded(m_level);
			m_state = state::loaded;
		} else {
			report_level_failed(m_level, m_handle);
			m_state = state::failed;
		}
	}
	return m_state;
}
