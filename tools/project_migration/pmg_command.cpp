#include <snk/project_migration/pmg_command.h>
#include <snk/project_migration/pmg_assets.h>
#include <snk/common/fs_paths.h>
#include <snk/common/logger/engine_log.h>
#include <snk/project/prj_json_text.h>
#include <snk/resource/res_desc_binary.h>
#include <snk/resource/res_shipping_dirs.h>
#include <snk/model_codec/res_tag.h>
#include <boost/json.hpp>
#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

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

	// Runs of digits compare as numbers, so "<time>-10" comes after "<time>-2".
	bool ids_in_order(std::string_view a, std::string_view b)
	{
		const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
		std::size_t i = 0;
		std::size_t j = 0;
		while (i < a.size() && j < b.size()) {
			if (is_digit(a[i]) && is_digit(b[j])) {
				std::size_t a_end = i;
				std::size_t b_end = j;
				while (a_end < a.size() && is_digit(a[a_end])) ++a_end;
				while (b_end < b.size() && is_digit(b[b_end])) ++b_end;
				std::string_view a_run = a.substr(i, a_end - i);
				std::string_view b_run = b.substr(j, b_end - j);
				const auto strip = [](std::string_view run) { return run.substr(std::min(run.find_first_not_of('0'), run.size())); };
				const std::string_view a_digits = strip(a_run);
				const std::string_view b_digits = strip(b_run);
				if (a_digits.size() != b_digits.size()) return a_digits.size() < b_digits.size();
				if (a_digits != b_digits) return a_digits < b_digits;
				if (a_run.size() != b_run.size()) return a_run.size() < b_run.size();
				i = a_end;
				j = b_end;
			} else {
				if (a[i] != b[j]) return a[i] < b[j];
				++i;
				++j;
			}
		}
		return a.size() - i < b.size() - j;
	}

	std::optional<std::string> write_report(const command_context& context, const fs::path& file, std::string_view text)
	{
		return context.write_report ? context.write_report(file, text) : prj::write_text_atomically(file, text);
	}
}

int prj::migration::exit_code_of(migration_status status)
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
	const bool renews = std::ranges::find(plan.project_steps, "assets.ids_below", &planned_project_step::id)
		!= plan.project_steps.end();
	bool copies = false;
	for (const scanned_file& file : plan.scan.files) {
		if (!renews || file.id_held_below.empty()) continue;
		if (!std::exchange(copies, true)) text += "New IDs (the ID belongs to a file of a layer below the project):\n";
		text += std::format("- {} ({}): ID of {}\n", file.path, file.id_held_below_meta, file.id_held_below);
	}
	bool to_copies = false;
	for (const scanned_file& file : plan.scan.files) {
		if (!renews || file.references_to_copies.empty()) continue;
		if (!std::exchange(to_copies, true)) text += "References turned into {guid, path} with a new ID of a copy:\n";
		for (const std::string& reference : file.references_to_copies) {
			text += std::format("- {}: {}\n", file.path, reference);
		}
	}
	const bool blobs = std::ranges::find(plan.project_steps, "engine.geometry_blobs", &planned_project_step::id)
		!= plan.project_steps.end();
	bool geometry = false;
	for (const scanned_file& file : plan.scan.files) {
		if (!blobs || !file.geometry_arrays) continue;
		if (!std::exchange(geometry, true)) text += "Geometry from JSON arrays to a binary file beside its desc:\n";
		const std::optional<res::tag> binary = res::desc_binary::binary_tag(res::tag{ file.path });
		text += std::format("- {} -> {}\n", file.path, binary ? binary->view() : std::string_view{});
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
		print_error(context, std::format("migration {} did not finish: run --migrate-rollback {} first", journal_path_text(unfinished.front()),
			journal_path_text(unfinished.front())));
		return k_exit_refused;
	}

	plan_options options;
	options.project_steps = context.project_steps;
	options.force = command.force;
	options.scan.read_only_layers = read_only_layers(context.resources, context.root);
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

	apply_context apply{ context.root, context.descs, context.resources, {}, {}, context.write, context.make_id, context.project_steps, {}, {} };
	const apply_outcome outcome = apply_migration(plan, apply);
	if (!outcome.refused.empty()) {
		print_error(context, std::format("refused: {}", outcome.refused));
		return k_exit_refused;
	}
	const std::string text = outcome.report.id.empty() ? plan_text(plan) : report_text(outcome.report);
	print(context, text);
	const int code = exit_code_of(outcome.report.status);
	if (!command.report.empty()) {
		if (auto error = write_report(context, command.report, text)) {
			print_error(context, std::format("--report {}: {}", fs_paths::utf8_of(command.report), *error));
			// A run whose report was asked for is not done without it; a failed migration keeps the code that says so.
			if (code == k_exit_done || code == k_exit_partial) return k_exit_refused;
		}
	}
	return code;
}

std::string prj::migration::journal_status(const fs::path& root, std::string_view id)
{
	const prj::project_file_text text = prj::read_project_file(root, fs_paths::path_from_utf8(std::format(".snk/migrations/{}/report.json", id)));
	if (!text.exists || !text.error.empty()) return {};
	boost::system::error_code error;
	const json::value journal = json::parse(text.text, error);
	if (error || !journal.is_object()) return {};
	const json::value* status = journal.as_object().if_contains("status");
	return status && status->is_string() ? std::string(status->as_string()) : std::string{};
}

int prj::migration::rollback_exit_code(const std::optional<std::string>& error, bool wrote)
{
	if (!error) return k_exit_done;
	return wrote ? k_exit_rollback_failed : k_exit_refused;
}

int prj::migration::run_migrate_rollback(std::string_view id, const command_context& context)
{
	bool wrote = false;
	rollback_context rollback;
	rollback.write = context.write;
	rollback.wrote = &wrote;
	if (const std::optional<std::string> error = rollback_migration(context.root, id, rollback)) {
		print_error(context, std::format("rollback of {}: {}", journal_path_text(id), *error));
		return rollback_exit_code(error, wrote);
	}
	print(context, std::format("migration {} rolled back", journal_path_text(id)));
	return k_exit_done;
}

int prj::migration::run_migrate_prune(const prune_command& command, const command_context& context)
{
	std::vector<migration_folder> finished;
	for (migration_folder& folder : migration_folders(context.root)) {
		if (!folder.problem.empty()) {
			print_error(context, std::format("migration {} is kept: {}", journal_path_text(folder.id), folder.problem));
			continue;
		}
		const bool is_finished = folder.status == to_string(migration_status::ok) || folder.status == to_string(migration_status::partial)
			|| folder.status == to_string(migration_status::rolled_back);
		if (is_finished) finished.push_back(std::move(folder));
		else if (folder.status != to_string(migration_status::in_progress) && folder.status != to_string(migration_status::rollback_failed)) {
			print_error(context, std::format("migration {} is kept: its status {} is not known", journal_path_text(folder.id),
				journal_path_text(folder.status)));
		}
	}
	std::ranges::sort(finished, [](const migration_folder& a, const migration_folder& b) {
		if (a.created != b.created) return a.created < b.created;
		return ids_in_order(a.id, b.id);
	});
	const std::size_t doomed = finished.size() > command.keep ? finished.size() - command.keep : 0;
	if (doomed == 0) {
		print(context, std::format("nothing to prune: {} finished migration(s), --keep {}", finished.size(), command.keep));
		return k_exit_done;
	}
	std::string list = std::format("{} {} of {} finished migration(s), the oldest:", command.dry_run ? "would delete" : "deleting", doomed,
		finished.size());
	for (std::size_t i = 0; i < doomed; ++i) {
		list += std::format("\n  {} ({}, {})", journal_path_text(finished[i].id), finished[i].created, finished[i].status);
	}
	print(context, list);
	if (command.dry_run) return k_exit_done;

	int code = k_exit_done;
	for (std::size_t i = 0; i < doomed; ++i) {
		const fs::path folder = context.root / fs_paths::path_from_utf8(std::format(".snk/migrations/{}", finished[i].id));
		std::error_code ec;
		const bool removed = context.remove_tree ? context.remove_tree(folder, ec) : core::res::remove_tree_without_following(folder, ec);
		if (!removed) {
			print_error(context, std::format("migration {} not deleted: {}", journal_path_text(finished[i].id), ec.message()));
			code = k_exit_refused;
		}
	}
	return code;
}
