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
#include <snk/project/prj_manifest.h>
#include <snk/project/prj_project.h>
#include <snk/resource/res_system.h>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <system_error>
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
	// The history (made below) lists an operation before its first step, so a crash leaves it to be undone at the next start.
	ops.operation_started = [this](const operation_journal& journal) {
		if (m_history) m_history->record_pending(journal);
	};
	ops.operation_ended = [this](const operation_journal& journal, bool done) {
		if (m_history) m_history->end_pending(journal, done);
	};
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
	for (auto& command : register_asset_commands(m_ports.context.actions().commands(), command_sources())) {
		out.push_back(std::move(command));
	}
	m_ports.browser.set_asset_operations(operation_ports());
	m_ports.browser.set_usages(
		[this, manifest_file, manifest_references, index = std::make_shared<usage_index>()](const res::tag& asset) -> std::optional<usage_report> {
			const std::vector<manifest_reference> manifest = manifest_references();
			return index->find(*m_database, m_ports.descs(), usage_target{ asset, std::nullopt }, manifest_file(), manifest);
		},
		[this, manifest_references] {
			return usage_stamp{ m_database->revision(), m_ports.game_layer_generation(), manifest_references() };
		});
}

edt::asset_command_sources edt::asset_component::command_sources()
{
	asset_command_sources sources;
	sources.operations = [this]() -> const asset_ops* { return m_ops.get(); };
	sources.write_blocker = m_ports.write_blocker;
	sources.asset_index_wait = [this] { return asset_index_wait(m_index.get()); };
	sources.done = [this](const operation_report& report) {
		m_history->record(report.journal);
		refresh_manifest();
	};
	sources.events = &m_ports.context.events();
	return sources;
}

edt::asset_operation_ports edt::asset_component::operation_ports()
{
	asset_operation_ports out;
	out.plan = [this](const asset_request& request) { return m_ops->plan(std::span<const asset_request>(&request, 1)); };
	out.run = [this](const asset_request& request, bool force) { return run_asset_operation(command_sources(), request, force); };
	out.undo = [this](const std::string& operation_id) { return undo_operation(operation_id); };
	return out;
}

std::string edt::refresh_manifest_references(const std::filesystem::path& manifest_file, prj::manifest& data)
{
	constexpr std::uintmax_t max_bytes = std::uintmax_t{ 1 } << 20;
	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(manifest_file, ec);
	if (ec) return std::format("the manifest cannot be read: {}", ec.message());
	if (size > max_bytes) return "the manifest is too big to be read again";
	std::ifstream in(manifest_file, std::ios::binary);
	const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	if (in.bad()) return "the manifest cannot be read";
	prj::manifest_result parsed = prj::parse_manifest(text);
	if (!parsed.data) return std::format("the manifest is not valid: {}", parsed.error ? prj::to_string(*parsed.error) : std::string("unknown"));
	data.start_scene = std::move(parsed.data->start_scene);
	data.input_actions = std::move(parsed.data->input_actions);
	return {};
}

void edt::asset_component::refresh_manifest()
{
	if (!m_ports.manifest_data) return;
	const std::string why = refresh_manifest_references(m_ports.project.manifest_file, *m_ports.manifest_data);
	if (!why.empty()) egLOG_WARN("asset", "the project's start scene and input actions were not read again: {}", why);
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
	const asset_fix_result result = m_history->undo_last(*m_ops);
	if (result.ok) refresh_manifest();
	return result;
}

edt::asset_fix_result edt::asset_component::undo_operation(const std::string& operation_id)
{
	if (const std::optional<std::string> blocked = m_ports.write_blocker ? m_ports.write_blocker() : std::nullopt) {
		return asset_fix_result{ false, *blocked, true };
	}
	if (m_history->next_operation_id() != std::optional<std::string>(operation_id) || !next_undo_is_operation()) {
		return asset_fix_result{ false, "something was done after this operation: take that back first (Edit > Undo)", true };
	}
	const asset_fix_result result = m_history->undo_last(*m_ops);
	if (result.ok) refresh_manifest();
	return result;
}
