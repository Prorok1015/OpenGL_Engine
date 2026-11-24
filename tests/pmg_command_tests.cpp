#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_folder_link.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/project/prj_json_text.h>
#include <boost/json.hpp>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
namespace pmg = prj::migration;

// The command line of a migration (#757, EPIC-58 S6) on copies of the fixture of old versions (#750).
namespace {

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << text;
}

// Every file of the tree, hidden ones (.snk, .cache) included.
std::map<std::string, std::string> snapshot(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		files[fs::relative(entry.path(), root).generic_string()] = entry.is_regular_file() ? read_text(entry.path()) : std::string{ "<dir>" };
	}
	return files;
}

struct command_fixture : test_util::scene_env
{
	command_fixture()
	{
		pmg::register_project_steps(steps);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });
	}

	pmg::command_context context()
	{
		pmg::command_context context{ copy.root(), *descs, *rs, &steps, write, [] { return std::string{ "test-migration" }; }, {}, {}, {} };
		context.print = [this](std::string_view text) { printed += std::string(text) + "\n"; };
		return context;
	}

	int migrate(bool dry_run, bool force, fs::path report = {})
	{
		pmg::migrate_command command;
		command.dry_run = dry_run;
		command.force = force;
		command.report = std::move(report);
		return pmg::run_migrate(command, context());
	}

	// The fixture without its invalid file and the file of an unloaded module: what migrates whole.
	void leave_only_what_migrates()
	{
		fs::remove(copy.res() / "broken.desc");
		fs::remove(copy.res() / "newer.desc");
		fs::remove(copy.res() / "module/unknown.desc");
	}

	test_util::fixture_copy copy{ "migration_v1" };
	pmg::project_step_registry steps;
	pmg::project_writer write;
	std::string printed;
};

}

BOOST_FIXTURE_TEST_SUITE(PmgCommandTests, command_fixture)

// --dry-run reads the project and writes nothing at all: no file changes, none appears (no .snk, no .cache), whatever
// the plan holds (outdated descs, a manifest of an older engine, assets.ids for files without a .resource).
BOOST_AUTO_TEST_CASE(ADryRun_WritesNothing_AndSaysAMigrationIsNeeded)
{
	const auto before = snapshot(copy.root());
	BOOST_TEST(migrate(true, true) == pmg::k_exit_migration_needed);
	BOOST_TEST((snapshot(copy.root()) == before));
	BOOST_TEST(printed.find("assets.ids") != std::string::npos, printed);
	BOOST_TEST(printed.find("broken.desc") != std::string::npos, printed);

	BOOST_TEST(migrate(true, false) == pmg::k_exit_refused);
	BOOST_TEST((snapshot(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(AnInvalidFile_WithoutForce_IsRefused_AndNothingIsWritten)
{
	const auto before = snapshot(copy.root());
	BOOST_TEST(migrate(false, false) == pmg::k_exit_refused);
	BOOST_TEST(printed.find("broken.desc") != std::string::npos, printed);
	BOOST_TEST((snapshot(copy.root()) == before));
}

// unknown_type and newer files are never migrated; --force leaves the invalid one as well: partial.
BOOST_AUTO_TEST_CASE(WithForce_WhatCannotMigrateIsLeft_Partial)
{
	const std::string broken = read_text(copy.res() / "broken.desc");
	const std::string newer = read_text(copy.res() / "newer.desc");
	BOOST_TEST(migrate(false, true) == pmg::k_exit_partial);
	BOOST_TEST(read_text(copy.res() / "broken.desc") == broken);
	BOOST_TEST(read_text(copy.res() / "newer.desc") == newer);
	BOOST_TEST(fs::exists(copy.root() / ".snk/migrations/test-migration/report.json"));
	BOOST_TEST(printed.find("Not migrated") != std::string::npos, printed);

	// What is left still cannot migrate; nothing else is to do.
	printed.clear();
	BOOST_TEST(migrate(true, true) == pmg::k_exit_partial, printed);
}

// A newer file alone is no reason to refuse (EPIC-58 4.3: code 4).
BOOST_AUTO_TEST_CASE(ANewerFile_IsLeftAsItIs_WithoutForce)
{
	fs::remove(copy.res() / "broken.desc");
	BOOST_TEST(migrate(false, false) == pmg::k_exit_partial, printed);
}

BOOST_AUTO_TEST_CASE(AProjectThatMigratesWhole_IsDone_ThenHasNothingToDo)
{
	leave_only_what_migrates();
	BOOST_TEST(migrate(false, false) == pmg::k_exit_done, printed);
	printed.clear();
	const auto after = snapshot(copy.root());
	BOOST_TEST(migrate(true, false) == pmg::k_exit_done, printed);
	BOOST_TEST(printed.find("Nothing to do") != std::string::npos, printed);
	BOOST_TEST(migrate(false, false) == pmg::k_exit_done, printed);
	BOOST_TEST((snapshot(copy.root()) == after));
}

BOOST_AUTO_TEST_CASE(AFailedMigration_IsRolledBack_Code5)
{
	leave_only_what_migrates();
	int content_writes = 0;
	write = [&content_writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (!relative.generic_string().starts_with(".snk/") && ++content_writes == 3) return std::string{ "injected failure" };
		return prj::write_project_file(root, relative, text);
	};
	BOOST_TEST(migrate(false, false) == pmg::k_exit_rolled_back, printed);
	BOOST_TEST(printed.find("injected failure") != std::string::npos, printed);
}

// A rollback that cannot finish leaves the project for --migrate-rollback: --migrate refuses until it has run.
BOOST_AUTO_TEST_CASE(ARollbackThatFails_IsCode6_AndTheNextRunIsRefusedUntilItIsRolledBack)
{
	leave_only_what_migrates();
	const auto before = snapshot(copy.root());
	int content_writes = 0;
	write = [&content_writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (!relative.generic_string().starts_with(".snk/") && ++content_writes >= 3) return std::string{ "injected failure" };
		return prj::write_project_file(root, relative, text);
	};
	BOOST_TEST(migrate(false, false) == pmg::k_exit_rollback_failed, printed);

	write = {};
	printed.clear();
	BOOST_TEST(migrate(true, false) == pmg::k_exit_refused);
	BOOST_TEST(printed.find("--migrate-rollback test-migration") != std::string::npos, printed);

	// A manual rollback whose restore fails too: code 6, the journal stays rollback_failed.
	write = [](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (!relative.generic_string().starts_with(".snk/")) return std::string{ "injected restore failure" };
		return prj::write_project_file(root, relative, text);
	};
	printed.clear();
	BOOST_TEST(pmg::run_migrate_rollback("test-migration", context()) == pmg::k_exit_rollback_failed, printed);
	BOOST_TEST(pmg::journal_status(copy.root(), "test-migration") == "rollback_failed");
	write = {};

	// A retry refused before it writes (a file changed since the migration wrote it) is code 2: nothing was written.
	std::string changed_path;
	std::string changed_text;
	for (const auto& [path, text] : snapshot(copy.root())) {
		if (path.starts_with(".snk") || path == "project.snkproj") continue;
		const auto was = before.find(path);
		if (was != before.end() && was->second != text) {
			changed_path = path;
			changed_text = text;
			break;
		}
	}
	BOOST_REQUIRE(!changed_path.empty());
	write_text(copy.root() / changed_path, changed_text + " ");
	const auto refused_state = snapshot(copy.root());
	printed.clear();
	BOOST_TEST(pmg::run_migrate_rollback("test-migration", context()) == pmg::k_exit_refused, printed);
	BOOST_TEST((snapshot(copy.root()) == refused_state));
	write_text(copy.root() / changed_path, changed_text);
	printed.clear();

	BOOST_TEST(pmg::run_migrate_rollback("test-migration", context()) == pmg::k_exit_done, printed);
	auto now = snapshot(copy.root());
	std::erase_if(now, [](const auto& entry) { return entry.first.starts_with(".snk"); });
	BOOST_TEST((now == before));
}

BOOST_AUTO_TEST_CASE(ARollbackById_PutsTheProjectBack)
{
	leave_only_what_migrates();
	const auto before = snapshot(copy.root());
	BOOST_REQUIRE(migrate(false, false) == pmg::k_exit_done);
	BOOST_TEST(pmg::run_migrate_rollback("test-migration", context()) == pmg::k_exit_done, printed);
	auto now = snapshot(copy.root());
	std::erase_if(now, [](const auto& entry) { return entry.first.starts_with(".snk"); });
	BOOST_TEST((now == before));
}

// A file changed after the migration: the rollback refuses, names it and puts nothing back.
BOOST_AUTO_TEST_CASE(ARollback_OfAFileChangedSince_IsRefused_NamingIt)
{
	leave_only_what_migrates();
	BOOST_REQUIRE(migrate(false, false) == pmg::k_exit_done);
	write_text(copy.res() / "cameras/cam_v1.desc", R"({"__type":"camera_desc","__version":2,"fov":10})");
	const auto changed = snapshot(copy.root());
	printed.clear();
	BOOST_TEST(pmg::run_migrate_rollback("test-migration", context()) == pmg::k_exit_refused);
	BOOST_TEST(printed.find("cameras/cam_v1.desc") != std::string::npos, printed);
	BOOST_TEST((snapshot(copy.root()) == changed));
}

BOOST_AUTO_TEST_CASE(ARollbackOfAnUnknownId_IsRefused)
{
	BOOST_TEST(pmg::run_migrate_rollback("no-such-migration", context()) == pmg::k_exit_refused);
}

// #761: the journal the caller checked is the one rolled back; one that reads otherwise by then writes nothing.
BOOST_AUTO_TEST_CASE(AJournalChangedSinceItWasChecked_IsRefused_NothingWritten)
{
	leave_only_what_migrates();
	BOOST_REQUIRE(migrate(false, false) == pmg::k_exit_done);
	const fs::path journal = copy.root() / ".snk/migrations/test-migration/report.json";
	const std::string checked = pmg::journal_hash(read_text(journal));
	write_text(journal, read_text(journal) + "\n");
	const auto before = snapshot(copy.root());
	bool wrote = false;
	pmg::rollback_context rollback;
	rollback.wrote = &wrote;
	rollback.journal_hash = checked;
	const std::optional<std::string> error = pmg::rollback_migration(copy.root(), "test-migration", rollback);
	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(error->find("changed since it was checked") != std::string::npos, *error);
	BOOST_TEST(!wrote);
	BOOST_TEST((snapshot(copy.root()) == before));
	BOOST_TEST(pmg::rollback_exit_code(error, wrote) == pmg::k_exit_refused);

	rollback.journal_hash = pmg::journal_hash(read_text(journal));
	BOOST_TEST(!pmg::rollback_migration(copy.root(), "test-migration", rollback).has_value());
	BOOST_TEST(wrote);
}

// --report never writes into the project (a dry run then would write after all); outside it is the printed text.
BOOST_AUTO_TEST_CASE(AReportInsideTheProject_IsRefused_OutsideItIsWritten)
{
	const auto before = snapshot(copy.root());
	BOOST_TEST(migrate(true, true, copy.root() / "plan.md") == pmg::k_exit_refused);
	BOOST_TEST(migrate(true, true, copy.res() / "../.cache/plan.md") == pmg::k_exit_refused);
	BOOST_TEST((snapshot(copy.root()) == before));

	test_util::fixture_copy outside{ "migration_v1" };
	printed.clear();
	BOOST_TEST(migrate(true, true, outside.root() / "plan.md") == pmg::k_exit_migration_needed);
	BOOST_TEST(read_text(outside.root() / "plan.md") + "\n" == printed);
	BOOST_TEST((snapshot(copy.root()) == before));
}

// The migration went through but its report could not be written: refused; a failed migration keeps its own code.
BOOST_AUTO_TEST_CASE(AReportThatCannotBeWritten_IsCode2_UnlessTheMigrationFailed)
{
	test_util::fixture_copy outside{ "migration_v1" };
	leave_only_what_migrates();
	bool report_written = false;
	const auto refuse_report = [&report_written](const fs::path&, std::string_view) -> std::optional<std::string> {
		report_written = true;
		return std::string{ "injected report failure" };
	};
	pmg::command_context with_failing_report = context();
	with_failing_report.write_report = refuse_report;
	pmg::migrate_command command;
	command.report = outside.root() / "report.md";
	BOOST_TEST(pmg::run_migrate(command, with_failing_report) == pmg::k_exit_refused, printed);
	BOOST_TEST(report_written);
	BOOST_TEST(printed.find("injected report failure") != std::string::npos, printed);
	BOOST_TEST(pmg::run_migrate_rollback("test-migration", context()) == pmg::k_exit_done, printed);

	int content_writes = 0;
	write = [&content_writes](const fs::path& root, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
		if (!relative.generic_string().starts_with(".snk/") && ++content_writes == 3) return std::string{ "injected failure" };
		return prj::write_project_file(root, relative, text);
	};
	pmg::command_context failing_too = context();
	failing_too.write_report = refuse_report;
	BOOST_TEST(pmg::run_migrate(command, failing_too) == pmg::k_exit_rolled_back, printed);
}

// #767: report.md and report.json carry the size of the backup; --migrate-prune --keep 0 deletes a real migration.
BOOST_AUTO_TEST_CASE(AMigration_ReportsItsBackupSize_AndPruneDeletesItsFolder)
{
	leave_only_what_migrates();
	BOOST_REQUIRE(migrate(false, false) == pmg::k_exit_done);
	const fs::path folder = copy.root() / ".snk/migrations/test-migration";
	const std::string md = read_text(folder / "report.md");
	BOOST_TEST(md.find(" files).") != std::string::npos, md);
	const boost::json::value journal = boost::json::parse(read_text(folder / "report.json"));
	const boost::json::value* bytes = journal.as_object().if_contains("backup_bytes");
	BOOST_REQUIRE(bytes);
	BOOST_TEST(bytes->to_number<std::uint64_t>() == pmg::size_of_tree(folder / "backup").bytes);
	BOOST_TEST(bytes->to_number<std::uint64_t>() > 0u);

	printed.clear();
	BOOST_TEST(pmg::run_migrate_prune({ 0, false }, context()) == pmg::k_exit_done, printed);
	BOOST_TEST(!fs::exists(folder));
}

BOOST_AUTO_TEST_CASE(SizeText_CountsInBinaryUnits)
{
	BOOST_TEST(pmg::size_text(0) == "0 B");
	BOOST_TEST(pmg::size_text(1023) == "1023 B");
	BOOST_TEST(pmg::size_text(1024) == "1.0 KiB");
	BOOST_TEST(pmg::size_text(1536) == "1.5 KiB");
	BOOST_TEST(pmg::size_text(1024u * 1024 - 1) == "1.0 MiB");
	BOOST_TEST(pmg::size_text(5u * 1024 * 1024) == "5.0 MiB");
	BOOST_TEST(pmg::size_text(std::uintmax_t{ 3 } * 1024 * 1024 * 1024 * 1024) == "3072.0 GiB");
}

namespace {

void write_journal(const fs::path& root, const std::string& id, const std::string& status, const std::string& created)
{
	std::string json = std::format(R"({{"id":"{}","status":"{}")", id, status);
	if (!created.empty()) json += std::format(R"(,"created":"{}")", created);
	write_text(root / ".snk/migrations" / id / "report.json", json + "}");
	write_text(root / ".snk/migrations" / id / "backup/res/a.desc", "{}");
}

}

// The newest by `created`, not by id, stay; unfinished ones and journals that cannot be trusted stay and do not count.
BOOST_AUTO_TEST_CASE(Prune_KeepsTheNewestFinished_AndNeverAnUnfinishedOrUntrustedOne)
{
	const fs::path root = copy.root();
	write_journal(root, "a-newest", "ok", "2026-03-01T00:00:00Z");
	write_journal(root, "b-oldest", "partial", "2026-01-01T00:00:00Z");
	write_journal(root, "c-middle", "rolled_back", "2026-02-01T00:00:00Z");
	write_journal(root, "d-stuck", "rollback_failed", "2025-01-01T00:00:00Z");
	write_journal(root, "e-running", "in_progress", "2025-01-01T00:00:00Z");
	write_journal(root, "f-no-time", "ok", "");
	write_journal(root, "g-bad-time", "ok", "yesterday");
	write_journal(root, "h-odd", "frozen", "2024-01-01T00:00:00Z");
	write_text(root / ".snk/migrations/i-broken/report.json", "{ not json");
	fs::create_directories(root / ".snk/migrations/j-empty");

	const auto before = snapshot(root);
	BOOST_TEST(pmg::run_migrate_prune({ 1, true }, context()) == pmg::k_exit_done, printed);
	BOOST_TEST((snapshot(root) == before));
	BOOST_TEST(printed.find("would delete 2 of 3") != std::string::npos, printed);
	BOOST_TEST(printed.find("b-oldest") < printed.find("c-middle"), printed);
	for (const char* named : { "f-no-time", "g-bad-time", "h-odd", "i-broken", "j-empty" }) {
		BOOST_TEST(printed.find(named) != std::string::npos, named << "\n" << printed);
	}

	printed.clear();
	BOOST_TEST(pmg::run_migrate_prune({ 1, false }, context()) == pmg::k_exit_done, printed);
	for (const char* gone : { "b-oldest", "c-middle" }) {
		BOOST_TEST(!fs::exists(root / ".snk/migrations" / gone), gone);
	}
	for (const char* kept : { "a-newest", "d-stuck", "e-running", "f-no-time", "g-bad-time", "h-odd", "i-broken", "j-empty" }) {
		BOOST_TEST(fs::exists(root / ".snk/migrations" / kept), kept);
	}

	printed.clear();
	BOOST_TEST(pmg::run_migrate_prune({ 1, false }, context()) == pmg::k_exit_done, printed);
	BOOST_TEST(printed.find("nothing to prune") != std::string::npos, printed);
}

// A link in .snk/migrations is not a migration folder: neither it nor what it points at is deleted.
BOOST_AUTO_TEST_CASE(Prune_NeverFollowsALink)
{
	test_util::fixture_copy outside{ "migration_v1" };
	write_journal(outside.root(), "elsewhere", "ok", "2026-01-01T00:00:00Z");
	fs::create_directories(copy.root() / ".snk/migrations");
	const fs::path link = copy.root() / ".snk/migrations/linked";
	std::error_code ec;
	fs::create_directory_symlink(outside.root() / ".snk/migrations/elsewhere", link, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("no symlink here: " << ec.message());
		return;
	}
	BOOST_TEST(pmg::run_migrate_prune({ 0, false }, context()) == pmg::k_exit_done, printed);
	BOOST_TEST(fs::is_symlink(fs::symlink_status(link)));
	BOOST_TEST(fs::exists(outside.root() / ".snk/migrations/elsewhere/report.json"));
}

// .snk or .snk/migrations a link: the journals behind it are not the project's, nothing there is deleted.
BOOST_AUTO_TEST_CASE(Prune_DeletesNothingBehindALinkedSnkOrMigrationsFolder)
{
	for (const char* linked : { ".snk", ".snk/migrations" }) {
		BOOST_TEST_INFO("linked: " << linked);
		test_util::fixture_copy project{ "migration_v1" };
		test_util::fixture_copy outside{ "migration_v1" };
		write_journal(outside.root(), "old", "ok", "2026-01-01T00:00:00Z");
		write_journal(outside.root(), "new", "ok", "2026-02-01T00:00:00Z");
		const fs::path target = std::string_view{ linked } == ".snk" ? outside.root() / ".snk" : outside.root() / ".snk/migrations";
		const fs::path link = project.root() / linked;
		fs::create_directories(link.parent_path());
		std::error_code ec;
		fs::create_directory_symlink(target, link, ec);
		if (ec) {
			BOOST_TEST_MESSAGE("no symlink here: " << ec.message());
			return;
		}
		pmg::command_context in_project = context();
		in_project.root = project.root();
		pmg::run_migrate_prune({ 0, false }, in_project);
		BOOST_TEST(fs::exists(outside.root() / ".snk/migrations/old/report.json"), printed);
		BOOST_TEST(fs::exists(outside.root() / ".snk/migrations/new/report.json"), printed);
	}
}

// Review of #1232: a `created` out of range is no time, it is kept and named; the same `created` orders ids by their
// numbers.
BOOST_AUTO_TEST_CASE(Prune_ACreatedOutOfRangeIsKept_TheSameCreatedOrdersIdsByNumber)
{
	const fs::path root = copy.root();
	write_journal(root, "real", "ok", "2026-01-01T00:00:00Z");
	for (const char* bad : { "9999-99-99T99:99:99Z", "2026-02-30T00:00:00Z", "2026-01-01T24:00:00Z", "2026-13-01T00:00:00Z" }) {
		write_journal(root, std::string{ "bad-" } + bad[5] + bad[6] + bad[8] + bad[9] + bad[11], "ok", bad);
	}
	write_journal(root, "20260301T000000Z-1.0.0-2", "ok", "2026-03-01T00:00:00Z");
	write_journal(root, "20260301T000000Z-1.0.0-10", "ok", "2026-03-01T00:00:00Z");

	BOOST_TEST(pmg::run_migrate_prune({ 1, false }, context()) == pmg::k_exit_done, printed);
	BOOST_TEST(fs::exists(root / ".snk/migrations/20260301T000000Z-1.0.0-10"), printed);
	BOOST_TEST(!fs::exists(root / ".snk/migrations/20260301T000000Z-1.0.0-2"), printed);
	BOOST_TEST(!fs::exists(root / ".snk/migrations/real"), printed);
	std::size_t bad_kept = 0;
	for (const auto& entry : fs::directory_iterator(root / ".snk/migrations")) {
		if (entry.path().filename().string().starts_with("bad-")) {
			++bad_kept;
			BOOST_TEST(printed.find(entry.path().filename().string()) != std::string::npos, printed);
		}
	}
	BOOST_TEST(bad_kept == 4u);
}

// #1316: a link inside a pruned migration folder is deleted as the link, what it points at stays.
BOOST_AUTO_TEST_CASE(Prune_DeletesALinkInsideAMigrationFolder_AsTheLink)
{
	test_util::fixture_copy outside{ "migration_v1" };
	write_journal(outside.root(), "elsewhere", "ok", "2026-01-01T00:00:00Z");
	const fs::path root = copy.root();
	write_journal(root, "a", "ok", "2026-01-01T00:00:00Z");
	BOOST_REQUIRE(test_util::link_folder(outside.root() / ".snk/migrations/elsewhere", root / ".snk/migrations/a/escape"));

	BOOST_TEST(pmg::run_migrate_prune({ 0, false }, context()) == pmg::k_exit_done, printed);
	BOOST_TEST(!fs::exists(fs::symlink_status(root / ".snk/migrations/a")));
	BOOST_TEST(fs::exists(outside.root() / ".snk/migrations/elsewhere/report.json"));
}

// A folder that cannot be deleted is named; the others still go, and the run says it did not finish.
BOOST_AUTO_TEST_CASE(Prune_AFolderThatCannotBeDeleted_IsNamed_TheOthersGo)
{
	const fs::path root = copy.root();
	write_journal(root, "a", "ok", "2026-01-01T00:00:00Z");
	write_journal(root, "b", "ok", "2026-01-02T00:00:00Z");
	write_journal(root, "c", "ok", "2026-01-03T00:00:00Z");
	pmg::command_context failing = context();
	failing.remove_tree = [](const fs::path& folder) -> std::string {
		if (folder.filename() == "a") {
			return "permission denied";
		}
		std::error_code ec;
		fs::remove_all(folder, ec);
		return ec ? ec.message() : std::string{};
	};
	BOOST_TEST(pmg::run_migrate_prune({ 1, false }, failing) == pmg::k_exit_refused, printed);
	BOOST_TEST(fs::exists(root / ".snk/migrations/a"));
	BOOST_TEST(!fs::exists(root / ".snk/migrations/b"));
	BOOST_TEST(fs::exists(root / ".snk/migrations/c"));
	BOOST_TEST(printed.find("migration a not deleted") != std::string::npos, printed);
}

BOOST_AUTO_TEST_SUITE_END()
