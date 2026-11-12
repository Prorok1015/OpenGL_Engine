#include <snk/game/gs_input_actions.h>
#include <snk/game/gs_input_bindings_file.h>
#include <snk/input/inp_actions.h>
#include <snk/project/prj_project.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/common/ds/ds_type_id.hpp>
#include <algorithm>
#include <cctype>
#include <format>

namespace
{
	// A name from project.snkproj is anything a repository says: the file is named by the hash of the raw name, with a short
	// ASCII prefix only for a person to tell the files apart (never a bare Windows device name, the hash follows a dash).
	std::string file_stem(std::string_view project_name)
	{
		std::string prefix;
		for (const char c : project_name) {
			if (prefix.size() == 24) break;
			if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80) prefix += c;
		}
		const std::string hash = std::format("{:016x}", ds::fnv1a_64(project_name));
		return prefix.empty() ? hash : prefix + "-" + hash;
	}
}

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
	m_project.reset();
	m_gamepad = {};
	m_user = {};
	m_user_file.clear();
	m_project_name.clear();
	m_changed = false;
	m_built = false;
	m_reported = false;
}

void gs::input_actions::update(const ds::app_data_storage& data)
{
	if (!data.has_value<prj::project_context>()) {
		update(std::string_view{});
		return;
	}
	const auto& manifest = data.require<prj::project_context>().data;
	update(std::string_view{ manifest.input_actions }, manifest.name);
}

void gs::input_actions::update(std::string_view tag)
{
	update(tag, {});
}

void gs::input_actions::set_user_folder(std::filesystem::path folder)
{
	m_user_folder = std::move(folder);
	load_user();
}

void gs::input_actions::load_user()
{
	m_user_file.clear();
	if (m_user_folder.empty() || m_project_name.empty()) return;
	m_user_file = m_user_folder / (file_stem(m_project_name) + ".json");
	m_user = load_user_bindings(m_user_file);
	if (m_project) rebuild_effective();
}

void gs::input_actions::rebuild_effective()
{
	m_map = std::make_shared<const inp::action_map>(m_user.apply(*m_project));
	// A file or an edit of the desc can leave one binding on two actions; rebind never does. Said once per rebuild, and
	// only for what the project's own map does not already have.
	const auto already = inp::find_binding_conflicts(*m_project);
	for (const auto& conflict : inp::find_binding_conflicts(*m_map)) {
		if (std::ranges::find(already, conflict) == already.end()) {
			egLOG_WARN("input", "actions '{}' and '{}' share a binding", conflict.first, conflict.second);
		}
	}
}

void gs::input_actions::save_user()
{
	m_saved = m_user_file.empty() || save_user_bindings(m_user_file, m_user);
}

gs::rebind_outcome gs::input_actions::rebind(std::string_view action, std::size_t slot, inp::action_binding binding)
{
	if (!m_project) return { { inp::rebind_status::unknown_action, {} }, true };
	const inp::rebind_result result = m_user.rebind(*m_project, action, slot, binding);
	if (!result.ok()) return { result, true };
	rebuild_effective();
	save_user();
	return { result, m_saved };
}

bool gs::input_actions::reset_binding(std::string_view action)
{
	if (!m_user.reset(action)) return false;
	if (m_project) rebuild_effective();
	save_user();
	return true;
}

void gs::input_actions::reset_all_bindings()
{
	if (m_user.empty()) return;
	m_user.reset_all();
	if (m_project) rebuild_effective();
	save_user();
}

void gs::input_actions::update(std::string_view tag, std::string_view project_name)
{
	if (tag != m_tag) {
		drop();
		if (tag.empty()) return;
		m_tag = std::string{ tag };
		m_project_name = std::string{ project_name };
		load_user();
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
		m_project = std::make_shared<const inp::action_map>(scn::build_action_map(*m_handle.get(), m_tag));
		m_gamepad = scn::build_gamepad_config(*m_handle.get(), m_tag);
		rebuild_effective();
		m_built = true;
		egLOG_INFO("input", "game actions from {}: {}", m_tag, m_map->size());
	} else if (m_handle.has_error() && !m_reported) {
		m_reported = true;
		egLOG_ERROR("input", "game actions {} did not load{}: {}", m_tag, m_map ? ", the previous ones stay" : "",
			m_handle.error_text());
	}
}
