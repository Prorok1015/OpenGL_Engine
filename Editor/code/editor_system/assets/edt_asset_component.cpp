#include "edt_asset_component.h"
#include "edt_asset_browser_panel.h"
#include "edt_asset_usages.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_system.h>
#include <utility>

edt::asset_component::asset_component(ports from)
	: m_ports(std::move(from))
{
	const prj::project_context& project = m_ports.project;
	m_database = std::make_unique<asset_database>(m_ports.context.jobs(), &m_ports.context.events(), m_ports.resources.get_resource_layers(),
		asset_cache_location{ project.root, project.cache_dir().lexically_relative(project.root) / "asset_db" });
	m_database->attach(m_ports.resources);
	m_database->set_vcs_busy(m_ports.vcs_busy);
	m_database->set_git_dir_provider(m_ports.git_dir);
	m_database->rebuild();
	m_catalog = std::make_unique<picker::database_catalog>(*m_database);
	m_index = std::make_unique<asset_index_link>(*m_database, m_ports.resources);
	m_problems = std::make_unique<asset_problem_publisher>();
}

edt::asset_component::~asset_component() = default;

void edt::asset_component::install(std::vector<registration>& out)
{
	asset_usage_sources usage_sources;
	usage_sources.assets = [this]() -> const asset_database* { return m_database.get(); };
	usage_sources.descs = m_ports.descs;
	const auto manifest_file = [manifest = &m_ports.project] { return fs_paths::utf8_of(manifest->manifest_file.filename()); };
	const auto manifest_references = [manifest = &m_ports.project] {
		std::vector<manifest_reference> refs;
		if (!manifest->data.start_scene.empty()) refs.push_back(manifest_reference{ "/start_scene", manifest->data.start_scene });
		if (!manifest->data.input_actions.empty()) refs.push_back(manifest_reference{ "/input_actions", manifest->data.input_actions });
		return refs;
	};
	usage_sources.manifest_file = manifest_file;
	usage_sources.manifest = manifest_references;
	for (auto& command : register_asset_usage_commands(m_ports.context.actions().commands(), std::move(usage_sources))) {
		out.push_back(std::move(command));
	}
	m_ports.browser.set_usages(
		[this, manifest_file, manifest_references, index = std::make_shared<usage_index>()](const res::tag& asset) -> std::optional<usage_report> {
			const std::vector<manifest_reference> manifest = manifest_references();
			return index->find(*m_database, m_ports.descs(), usage_target{ asset, std::nullopt }, manifest_file(), manifest);
		},
		[this, manifest_references] {
			return usage_stamp{ m_database->revision(), m_ports.game_layer_generation(), manifest_references() };
		});
}

void edt::asset_component::tick()
{
	// New files waiting for an ID: a git operation that held them back may have ended (#765).
	m_database->poll();
	m_index->poll();
	m_problems->publish(m_ports.context.problems(), *m_database);
}

std::vector<edt::problem_action> edt::asset_component::problem_actions(const problem& row)
{
	return m_problems->actions_of(row, *m_database);
}
