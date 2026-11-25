#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_document_parts.h"
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <filesystem>
#include <fstream>
#include <optional>
#include <snk/resource/res_system.h>
#include <string>
#include <vector>

// #1400 (EPIC-82 step a): the parts the file documents share for "the file changed on disk": the flag and its serial (each
// document keeps its own serial policy), the decision of a document that watches one file, the overwrite, the watch.

BOOST_AUTO_TEST_SUITE(EdtDocumentParts)

BOOST_AUTO_TEST_CASE(NoSerial_AFlagMovesNothing)
{
	edt::disk_state state(edt::disk_serial::none);
	BOOST_TEST(!state.changed());
	state.flag("v1");
	state.flag("v2");
	BOOST_TEST(state.changed());
	BOOST_TEST(state.serial() == 0u);
	state.set_changed(false);
	BOOST_TEST(!state.changed());
}

BOOST_AUTO_TEST_CASE(EveryFlagSerial_EachFlagMovesIt_WhateverTheVersion)
{
	edt::disk_state state(edt::disk_serial::every_flag);
	state.flag();
	state.flag();
	BOOST_TEST(state.serial() == 2u);
	state.set_changed(false);
	BOOST_TEST(!state.changed());
	BOOST_TEST(state.serial() == 2u);
	state.flag();
	BOOST_TEST(state.serial() == 3u);
}

BOOST_AUTO_TEST_CASE(PerVersionSerial_MovesOnlyForAVersionNotFlaggedLast_AndClearForgetsIt)
{
	edt::disk_state state(edt::disk_serial::per_version);
	state.flag("v1");
	BOOST_TEST(state.serial() == 1u);
	state.flag("v1");
	BOOST_TEST(state.serial() == 1u);
	state.flag("v2");
	BOOST_TEST(state.serial() == 2u);

	state.set_changed(false);
	state.flag("v2");
	BOOST_TEST(state.serial() == 2u);

	state.clear();
	BOOST_TEST(!state.changed());
	state.flag("v2");
	BOOST_TEST(state.serial() == 3u);
}

struct source_changed_run
{
	bool reload_result = true;
	int reloads = 0;
	int lock_asked = 0;
	bool locked = false;
	std::vector<std::string> log;

	void run(edt::disk_state& state, std::string_view on_disk, std::string_view read, bool dirty)
	{
		state.on_source_changed(on_disk, read, dirty,
			[this] {
				++lock_asked;
				return locked;
			},
			[this] {
				++reloads;
				log.push_back("reload");
				return reload_result;
			},
			[&state, this] { log.push_back(state.changed() ? "announce:flagged" : "announce:clear"); });
	}
};

BOOST_AUTO_TEST_CASE(SameVersion_ClearsAFlagThatWasSet_AndAnnouncesIt_NothingOtherwise)
{
	edt::disk_state state;
	source_changed_run run;
	run.run(state, "v1", "v1", true);
	BOOST_TEST(run.log.empty());

	state.set_changed(true);
	run.run(state, "v1", "v1", true);
	BOOST_TEST(!state.changed());
	BOOST_TEST((run.log == std::vector<std::string>{ "announce:clear" }), boost::test_tools::per_element());
	BOOST_TEST(run.reloads == 0);
	BOOST_TEST(run.lock_asked == 0);
}

BOOST_AUTO_TEST_CASE(AnotherVersion_OfACleanUnlockedDocument_IsReloaded_AndFlagsNothing)
{
	edt::disk_state state;
	source_changed_run run;
	run.run(state, "v2", "v1", false);
	BOOST_TEST(run.reloads == 1);
	BOOST_TEST(run.lock_asked == 1);
	BOOST_TEST(!state.changed());
	BOOST_TEST((run.log == std::vector<std::string>{ "reload" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(AnotherVersion_OfADirtyDocument_IsFlaggedAndAnnouncedOnce_WithoutAskingTheLockOrReloading)
{
	edt::disk_state state;
	source_changed_run run;
	run.run(state, "v2", "v1", true);
	BOOST_TEST(state.changed());
	BOOST_TEST(run.lock_asked == 0);
	BOOST_TEST(run.reloads == 0);
	BOOST_TEST((run.log == std::vector<std::string>{ "announce:flagged" }), boost::test_tools::per_element());

	run.run(state, "v2", "v1", true);
	BOOST_TEST(run.log.size() == 1u);
}

BOOST_AUTO_TEST_CASE(AnotherVersion_OfALockedDocument_IsFlagged_AndAReloadThatFails_FallsBackToTheFlag)
{
	edt::disk_state locked_state;
	source_changed_run locked;
	locked.locked = true;
	locked.run(locked_state, "v2", "v1", false);
	BOOST_TEST(locked.lock_asked == 1);
	BOOST_TEST(locked.reloads == 0);
	BOOST_TEST(locked_state.changed());

	edt::disk_state failed_state;
	source_changed_run failed;
	failed.reload_result = false;
	failed.run(failed_state, "v2", "v1", false);
	BOOST_TEST((failed.log == std::vector<std::string>{ "reload", "announce:flagged" }), boost::test_tools::per_element());
	BOOST_TEST(failed_state.changed());
}

BOOST_AUTO_TEST_CASE(Overwrite_TakesTheDiskVersionWhileItSaves_AndKeepsItWhenTheSaveWorked)
{
	edt::disk_state state;
	std::string version = "mine";
	std::string seen_during_save;
	const edt::save_result saved = state.overwrite(version, "theirs",
		[&] {
			seen_during_save = version;
			return edt::save_result::ok();
		},
		[](const edt::save_result& result) { return !result.saved; });
	BOOST_TEST(saved.saved);
	BOOST_TEST(seen_during_save == "theirs");
	BOOST_TEST(version == "theirs");
	BOOST_TEST(!state.changed());
}

BOOST_AUTO_TEST_CASE(Overwrite_ThatFails_PutsTheVersionBack_AndFlagsAgain)
{
	edt::disk_state state;
	std::string version = "mine";
	const edt::save_result saved = state.overwrite(version, "theirs",
		[] { return edt::save_result::failure("locked"); },
		[](const edt::save_result& result) { return !result.saved; });
	BOOST_TEST(!saved.saved);
	BOOST_TEST(saved.error == "locked");
	BOOST_TEST(version == "mine");
	BOOST_TEST(state.changed());
}

BOOST_AUTO_TEST_CASE(Overwrite_WhoseFailureIsNotTheDocumentsOwn_IsKept)
{
	edt::disk_state state;
	std::string version = "mine";
	const edt::save_result saved = state.overwrite(version, "theirs",
		[] { return edt::save_result::failure("the geometry failed"); },
		[](const edt::save_result&) { return false; });
	BOOST_TEST(!saved.saved);
	BOOST_TEST(version == "theirs");
	BOOST_TEST(!state.changed());
}

struct watch_env : test_util::scene_env {};

BOOST_FIXTURE_TEST_CASE(SourceWatch_CallsBackForItsTag_AndNotAfterUnwatch_OrForAReplacedWatch, watch_env)
{
	const res::tag first = res::tag::make("a/first.desc");
	const res::tag second = res::tag::make("a/second.desc");
	int first_calls = 0;
	int second_calls = 0;
	int owner = 0;
	edt::source_watch watch;
	watch.watch(rs.get(), first, &owner, [&first_calls](const res::tag&) { ++first_calls; });
	rs->signal_changed(first);
	pump();
	BOOST_TEST(first_calls == 1);

	watch.watch(rs.get(), second, &owner, [&second_calls](const res::tag&) { ++second_calls; });
	rs->signal_changed(first);
	rs->signal_changed(second);
	pump();
	BOOST_TEST(first_calls == 1);
	BOOST_TEST(second_calls == 1);

	watch.unwatch();
	watch.unwatch();
	rs->signal_changed(second);
	pump();
	BOOST_TEST(second_calls == 1);
}

BOOST_AUTO_TEST_CASE(SourceWatch_WithoutResources_WatchesNothing)
{
	int owner = 0;
	edt::source_watch watch;
	watch.watch(nullptr, res::tag::make("a/b.desc"), &owner, [](const res::tag&) {});
	watch.unwatch();
	BOOST_TEST(true);
}

struct gate_env : test_util::scene_env
{
	gate_env()
	{
		std::filesystem::create_directories(res_root / "a");
		std::filesystem::create_directories(lower / "a");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<std::filesystem::path>{ lower, res_root },
			res::mount_access::read_write, res::write_policy::allow_all());
		layout.top_layer = [this] { return res_root; };
		layout.layers = [this] { return std::vector<std::filesystem::path>{ lower, res_root }; };
		layout.open_level_files = [] { return std::vector<std::string>{}; };
	}

	~gate_env() { rs->unregistrate_resolver(res::tag::default_protocol()); }

	static void write(const std::filesystem::path& root, std::string_view name, std::string_view content)
	{
		std::ofstream out(root / std::string(name), std::ios::binary);
		out << content;
	}

	edt::file_write_check check(const res::tag& tag, std::string_view version, edt::disk_state& state, int& announced)
	{
		return edt::write_gate::check(*rs, *descs, layout, tag, version, state, [&announced] { ++announced; });
	}

	test_util::temp_dir dir{ "edt_write_gate" };
	const std::filesystem::path res_root = dir.path() / "res";
	const std::filesystem::path lower = dir.path() / "lower";
	edt::desc_file_layout layout;
};

BOOST_AUTO_TEST_CASE(WriteGate_Blocker_IsThePortsAnswer_AndNothingWithoutAPort)
{
	edt::desc_file_layout layout;
	BOOST_TEST(!edt::write_gate::blocker(layout).has_value());
	layout.write_blocker = [] { return std::optional<std::string>{}; };
	BOOST_TEST(!edt::write_gate::blocker(layout).has_value());
	layout.write_blocker = [] { return std::optional<std::string>{ "a migration runs" }; };
	BOOST_TEST(edt::write_gate::blocker(layout).value_or("") == "a migration runs");
}

BOOST_FIXTURE_TEST_CASE(WriteGate_Check_OfTheVersionTheDocumentRead_AllowsAndFlagsNothing, gate_env)
{
	write(res_root, "a/x.desc", R"({"__type":"prefab_desc","__version":5})");
	const res::tag tag = res::tag::make("a/x.desc");
	edt::disk_state state;
	int announced = 0;
	const std::string read_version = check(tag, "", state, announced).version;
	state.set_changed(false);
	announced = 0;

	const edt::file_write_check allowed = check(tag, read_version, state, announced);
	BOOST_TEST(allowed.error.empty());
	BOOST_TEST(!allowed.changed_on_disk);
	BOOST_TEST(!state.changed());
	BOOST_TEST(announced == 0);
}

BOOST_FIXTURE_TEST_CASE(WriteGate_Check_OfAFileThatChanged_RefusesFlagsAndAnnouncesOnce, gate_env)
{
	write(res_root, "a/x.desc", R"({"__type":"prefab_desc","__version":5})");
	const res::tag tag = res::tag::make("a/x.desc");
	edt::disk_state state;
	int announced = 0;
	const std::string read_version = check(tag, "", state, announced).version;
	state.set_changed(false);
	announced = 0;
	write(res_root, "a/x.desc", R"({"__type":"prefab_desc","__version":5,"name":"outside"})");

	const edt::file_write_check refused = check(tag, read_version, state, announced);
	BOOST_TEST(refused.changed_on_disk);
	BOOST_TEST(refused.error.find("changed on disk") != std::string::npos);
	BOOST_TEST(state.changed());
	BOOST_TEST(announced == 1);

	check(tag, read_version, state, announced);
	BOOST_TEST(announced == 1);
}

BOOST_FIXTURE_TEST_CASE(WriteGate_Check_OfALowerLayerFile_RefusesWithoutTouchingTheFlag, gate_env)
{
	write(lower, "a/low.desc", R"({"__type":"prefab_desc","__version":5})");
	edt::disk_state state;
	int announced = 0;

	const edt::file_write_check refused = check(res::tag::make("a/low.desc"), "", state, announced);
	BOOST_TEST(refused.error.find("lower layer") != std::string::npos);
	BOOST_TEST(!state.changed());
	BOOST_TEST(announced == 0);
}

BOOST_AUTO_TEST_SUITE_END()
