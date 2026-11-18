#include "edt_asset_component.h"
#include "edt_asset_browser_panel.h"
#include "edt_asset_commands.h"
#include "edt_asset_history.h"
#include "edt_asset_ops.h"
#include "edt_asset_usages.h"
#include "edt_data_protocol.h"
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_editor_context.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_system.h>
#include <memory>
#include <span>
#include <utility>

namespace
{
	std::vector<edt::manifest_reference> manifest_references_of(const prj::project_context& project)
	{
		std::vector<edt::manifest_reference> refs;
		if (!project.data.start_scene.empty()) refs.push_back(edt::manifest_reference{ "/start_scene", project.data.start_scene });
		if (!project.data.input_actions.empty()) refs.push_back(edt::manifest_reference{ "/input_actions", project.data.input_actions });
		return refs;
	}
}

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

	asset_ops_ports ops;
	ops.assets = [this]() -> const asset_database* { return m_database.get(); };
	ops.descs = m_ports.descs;
	ops.manifest = [shown = &m_ports.project]() -> std::optional<asset_ops_manifest> {
		return asset_ops_manifest{ shown->manifest_file.parent_path(), fs_paths::utf8_of(shown->manifest_file.filename()), manifest_references_of(*shown) };
	};
	const std::filesystem::path ops_engine_base = res::resource_system::configured_engine_base();
	ops.engine_base = ops_engine_base;
	const std::filesystem::path journal_root = project.cache_dir() / "asset_ops";
	ops.journal_root = journal_root;
	ops.is_open_with_unsaved_changes = m_ports.is_open_with_unsaved_changes;
	ops.hold_changes = [this]() -> std::shared_ptr<void> { return std::make_shared<res::change_hold>(m_ports.resources.hold_changes()); };
	ops.report_self_change = [this](std::span<const res::file_change> changes) { m_database->report_self_change(changes); };
	m_ops = std::make_unique<asset_ops>(std::move(ops));

	// Which journals are the editor's own is kept in the person's data, not in the project's cache folder.
	m_history = std::make_unique<asset_history>(journal_root,
		asset_history_file(m_ports.resources.mount_roots("edt"), default_user_data_dir(), journal_root), [this] { return m_database->next_history_serial(); });
	m_history->restore(m_ops->changeable_roots());
}

edt::asset_component::~asset_component() = default;

void edt::asset_component::install(std::vector<registration>& out)
{
	asset_usage_sources usage_sources;
	usage_sources.assets = [this]() -> const asset_database* { return m_database.get(); };
	usage_sources.descs = m_ports.descs;
	const auto manifest_file = [manifest = &m_ports.project] { return fs_paths::utf8_of(manifest->manifest_file.filename()); };
	const auto manifest_references = [manifest = &m_ports.project] { return manifest_references_of(*manifest); };
	usage_sources.manifest_file = manifest_file;
	usage_sources.manifest = manifest_references;
	for (auto& command : register_asset_usage_commands(m_ports.context.actions().commands(), std::move(usage_sources))) {
		out.push_back(std::move(command));
	}
	asset_command_sources command_sources;
	command_sources.operations = [this]() -> const asset_ops* { return m_ops.get(); };
	command_sources.write_blocker = m_ports.write_blocker;
	command_sources.asset_index_wait = [this] { return asset_index_wait(m_index.get()); };
	command_sources.done = [this](const operation_report& report) { m_history->record(report.journal); };
	command_sources.events = &m_ports.context.events();
	for (auto& command : register_asset_commands(m_ports.context.actions().commands(), std::move(command_sources))) {
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

bool edt::asset_component::next_undo_is_operation() const
{
	return asset_history::newer_than(m_history->next_serial(), m_database->next_undo_serial());
}

std::optional<std::string> edt::asset_component::next_undo_label() const
{
	return next_undo_is_operation() ? m_history->next_label() : m_database->next_undo_label();
}

edt::asset_fix_result edt::asset_component::undo_last()
{
	if (!next_undo_is_operation()) return m_database->undo_last();
	if (const std::optional<std::string> blocked = m_ports.write_blocker ? m_ports.write_blocker() : std::nullopt) {
		return asset_fix_result{ false, *blocked, true };
	}
	return m_history->undo_last(*m_ops);
}
