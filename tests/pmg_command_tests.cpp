#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_scene_env.hpp"
#include <snk/project_migration/pmg_asset_steps.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/project/prj_json_text.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
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
		pmg::command_context context{ copy.root(), *descs, *rs, &steps, write, [] { return std::string{ "test-migration" }; }, {}, {} };
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

BOOST_AUTO_TEST_SUITE_END()
