#include <snk/project_migration/pmg_command.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
namespace json = boost::json;

namespace
{
	using namespace prj::migration;

	void print(const command_context& context, std::string_view text)
	{
		if (context.print) {
			context.print(text);
		} else {
			egLOG_INFO("project/migrate", "{}", text);
		}
	}

	void print_error(const command_context& context, std::string_view text)
	{
		if (context.print) {
			context.print(text);
		} else {
			egLOG_ERROR("project/migrate", "{}", text);
		}
	}

	bool is_within(const fs::path& path, const fs::path& folder)
	{
		std::error_code ec;
		const fs::path inner = fs::weakly_canonical(fs::absolute(path, ec), ec);
		const fs::path outer = fs::weakly_canonical(fs::absolute(folder, ec), ec);
		auto mismatch = std::mismatch(outer.begin(), outer.end(), inner.begin(), inner.end());
		return mismatch.first == outer.end();
	}

	std::optional<std::string> write_report(const command_context& context, const fs::path& file, std::string_view text)
	{
		return context.write_report ? context.write_report(file, text) : prj::write_text_atomically(file, text);
	}

	std::string journal_status(const fs::path& root, std::string_view id)
	{
		const prj::project_file_text text = prj::read_project_file(root, fs_paths::path_from_utf8(std::format(".snk/migrations/{}/report.json", id)));
		if (!text.exists || !text.error.empty()) return {};
		boost::system::error_code error;
		const json::value journal = json::parse(text.text, error);
		if (error || !journal.is_object()) return {};
		const json::value* status = journal.as_object().if_contains("status");
		return status && status->is_string() ? std::string(status->as_string()) : std::string{};
	}

	int exit_code_of(migration_status status)
	{
		switch (status) {
		case migration_status::ok: return k_exit_done;
		case migration_status::partial: return k_exit_partial;
		case migration_status::rolled_back: return k_exit_rolled_back;
		case migration_status::rollback_failed:
		case migration_status::in_progress: return k_exit_rollback_failed;
		}
		return k_exit_rollback_failed;
	}
}

std::string prj::migration::plan_text(const migration_plan& plan)
{
	std::string text = std::format("Migration plan: {} files, {} project steps\n", plan.files.size(), plan.project_steps.size());
	const manifest_state& manifest = plan.scan.manifest;
	if (plan.manifest_outdated()) {
		text += std::format("- project.snkproj: engine {}{}\n", manifest.engine_version,
			plan.completes() ? " -> this engine" : " (kept: files of an unloaded module stay)");
	}
	for (const planned_project_step& step : plan.project_steps) {
		text += std::format("- step {} ({}): {}\n", step.id, step.phase, step.summary);
	}
	for (const file_plan& file : plan.files) {
		text += std::format("- {}\n", file.path);
		for (const step_use& step : file.steps) {
			text += std::format("  - {} {} -> {}: {}\n", step.type, step.from_version, step.from_version + 1, step.summary);
		}
		for (const desc::migration_warning& warning : file.warnings) {
			text += std::format("  - warning: {} {} -> {}: {}\n", warning.type, warning.from_version, warning.from_version + 1, warning.message);
		}
	}
	const std::vector<skipped_file> skipped = skipped_by(plan);
	if (!skipped.empty()) {
		text += "Not migrated:\n";
		for (const skipped_file& file : skipped) text += std::format("- {}: {} {}\n", file.path, to_string(file.kind), file.reason);
	}
	if (plan.empty()) text += "Nothing to do.\n";
	return text;
}

int prj::migration::run_migrate(const migrate_command& command, const command_context& context)
{
	if (!command.report.empty() && is_within(command.report, context.root)) {
		print_error(context, std::format("--report {}: the report is not written inside the project", fs_paths::utf8_of(command.report)));
		return k_exit_refused;
	}
	// Before anything else, a dry run too: the project is partly migrated until that one is rolled back (EPIC-58 5).
	if (const std::vector<std::string> unfinished = unfinished_migrations(context.root); !unfinished.empty()) {
		print_error(context, std::format("migration {} did not finish: run --migrate-rollback {} first", unfinished.front(), unfinished.front()));
		return k_exit_refused;
	}

	plan_options options;
	options.project_steps = context.project_steps;
	options.force = command.force;
	const migration_plan plan = plan_migration(context.root, scan_project(context.root, context.descs, options.scan), context.descs, options);
	if (const std::string refused = plan_refusal(plan); !refused.empty()) {
		print_error(context, std::format("refused: {}", refused));
		return k_exit_refused;
	}

	if (command.dry_run) {
		const std::string text = plan_text(plan);
		print(context, text);
		if (!command.report.empty()) {
			if (auto error = write_report(context, command.report, text)) {
				print_error(context, std::format("--report {}: {}", fs_paths::utf8_of(command.report), *error));
				return k_exit_refused;
			}
		}
		if (!plan.empty()) return k_exit_migration_needed;
		return skipped_by(plan).empty() ? k_exit_done : k_exit_partial;
	}

	apply_context apply{ context.root, context.descs, context.resources, {}, {}, context.write, context.make_id, context.project_steps };
	const apply_outcome outcome = apply_migration(plan, apply);
	if (!outcome.refused.empty()) {
		print_error(context, std::format("refused: {}", outcome.refused));
		return k_exit_refused;
	}
	const std::string text = outcome.report.id.empty() ? plan_text(plan) : report_text(outcome.report);
	print(context, text);
	if (!command.report.empty()) {
		if (auto error = write_report(context, command.report, text)) {
			print_error(context, std::format("--report {}: {}", fs_paths::utf8_of(command.report), *error));
		}
	}
	return exit_code_of(outcome.report.status);
}

int prj::migration::run_migrate_rollback(std::string_view id, const command_context& context)
{
	rollback_context rollback;
	rollback.write = context.write;
	if (const std::optional<std::string> error = rollback_migration(context.root, id, rollback)) {
		print_error(context, std::format("rollback of {}: {}", id, *error));
		return journal_status(context.root, id) == to_string(migration_status::rollback_failed) ? k_exit_rollback_failed : k_exit_refused;
	}
	print(context, std::format("migration {} rolled back", id));
	return k_exit_done;
}
