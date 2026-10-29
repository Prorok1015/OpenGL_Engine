#include <boost/test/unit_test.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resolvers/res_file_watcher.h>
// Not test_scene_env.hpp: res_vfs_resolver.h brings in Asio and so <windows.h>.
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// remove, rename and the change events of code:// (#450), and the watcher's diff behind them.
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

std::vector<std::byte> to_bytes(std::string_view s)
{
	std::vector<std::byte> out(s.size());
	for (std::size_t i = 0; i < s.size(); ++i) out[i] = static_cast<std::byte>(s[i]);
	return out;
}

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct ops_fixture
{
	test_util::temp_dir dir{ "res_code_ops" };
	fs::path project = dir.path() / "project";
	fs::path content = project / "res";
	fs::path code = project / "code";
	fs::path cache = project / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> sys;
	std::vector<res::file_change> seen;
	int owner = 0;

	ops_fixture()
	{
		fs::create_directories(content / "levels");
		fs::create_directories(code / "src");
		fs::create_directories(cache / "memory");
		dir.write("project/res/levels/main.desc", "level");
		dir.write("project/code/src/game.cpp", "int game;");
		dir.write("project/code/src/notes.md", "notes");
		sys = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table({}, { content }, cache, cache / "user_profile", code));
		sys->subscribe_changes(res::tag::code, &owner, [this](std::span<const res::file_change> batch) {
			seen.insert(seen.end(), batch.begin(), batch.end());
		});
	}

	~ops_fixture()
	{
		sys->unsubscribe_changes(&owner);
	}

	template<class Predicate>
	bool pump_until(Predicate predicate, std::chrono::milliseconds limit = 8000ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!predicate()) {
			test_util::pump(tasks);
			if (std::chrono::steady_clock::now() > deadline) {
				return predicate();
			}
			std::this_thread::sleep_for(20ms);
		}
		return true;
	}

	bool has(res::change_kind kind, std::string_view tag, bool self) const
	{
		return std::ranges::any_of(seen, [&](const res::file_change& c) {
			return c.kind == kind && c.tag.view() == tag && c.self == self;
		});
	}

	bool has_tag(std::string_view tag) const
	{
		return std::ranges::any_of(seen, [&](const res::file_change& c) { return c.tag.view() == tag; });
	}
};

res::detail::watch_file_state state_of(std::uintmax_t size, int seconds)
{
	res::detail::watch_file_state state;
	state.size = size;
	state.modified = fs::file_time_type{} + std::chrono::seconds(seconds);
	return state;
}

}

BOOST_AUTO_TEST_SUITE(ResWatcherDiff)

BOOST_AUTO_TEST_CASE(NothingChanged_NothingReported)
{
	res::detail::watch_snapshot before;
	before.files["/c/a.cpp"] = state_of(10, 1);
	before.dirs.insert("/c/src");
	BOOST_TEST(res::detail::diff_snapshots(before, before).empty());
}

BOOST_AUTO_TEST_CASE(CreatedModifiedDeleted_AreTold)
{
	res::detail::watch_snapshot before;
	before.files["/c/kept.cpp"] = state_of(10, 1);
	before.files["/c/edited.cpp"] = state_of(10, 1);
	before.files["/c/gone.cpp"] = state_of(33, 5);
	res::detail::watch_snapshot after;
	after.files["/c/kept.cpp"] = state_of(10, 1);
	after.files["/c/edited.cpp"] = state_of(11, 1);
	after.files["/c/new.cpp"] = state_of(77, 9);

	const auto batch = res::detail::diff_snapshots(before, after);
	BOOST_REQUIRE(batch.size() == 3u);
	BOOST_TEST((batch[0].kind == res::change_kind::deleted && batch[0].path == "/c/gone.cpp"));
	BOOST_TEST((batch[1].kind == res::change_kind::created && batch[1].path == "/c/new.cpp"));
	BOOST_TEST((batch[2].kind == res::change_kind::modified && batch[2].path == "/c/edited.cpp"));
}

BOOST_AUTO_TEST_CASE(OneVanishedOneAppearedWithTheSameSizeAndTime_IsARename)
{
	res::detail::watch_snapshot before;
	before.files["/c/old.cpp"] = state_of(42, 7);
	res::detail::watch_snapshot after;
	after.files["/c/new.cpp"] = state_of(42, 7);

	const auto batch = res::detail::diff_snapshots(before, after);
	BOOST_REQUIRE(batch.size() == 1u);
	BOOST_TEST((batch[0].kind == res::change_kind::renamed));
	BOOST_TEST((batch[0].path == "/c/new.cpp"));
	BOOST_TEST((batch[0].old_path == "/c/old.cpp"));
}

BOOST_AUTO_TEST_CASE(TwoWithTheSameSizeAndTime_AreNotMatchedByAGuess)
{
	res::detail::watch_snapshot before;
	before.files["/c/a.cpp"] = state_of(42, 7);
	before.files["/c/b.cpp"] = state_of(42, 7);
	res::detail::watch_snapshot after;
	after.files["/c/c.cpp"] = state_of(42, 7);
	after.files["/c/d.cpp"] = state_of(42, 7);

	const auto batch = res::detail::diff_snapshots(before, after);
	BOOST_REQUIRE(batch.size() == 4u);
	for (const auto& change : batch) {
		BOOST_TEST((change.kind == res::change_kind::deleted || change.kind == res::change_kind::created));
	}
}

BOOST_AUTO_TEST_CASE(OneVanishedTwoAppearedWithTheSameKey_IsNotARename)
{
	res::detail::watch_snapshot before;
	before.files["/c/a.cpp"] = state_of(42, 7);
	res::detail::watch_snapshot after;
	after.files["/c/b.cpp"] = state_of(42, 7);
	after.files["/c/c.cpp"] = state_of(42, 7);

	const auto batch = res::detail::diff_snapshots(before, after);
	BOOST_REQUIRE(batch.size() == 3u);
	BOOST_TEST(std::ranges::none_of(batch, [](const res::raw_change& c) { return c.kind == res::change_kind::renamed; }));
}

BOOST_AUTO_TEST_CASE(Folders_AreReportedCreatedBeforeTheirFilesAndDeletedAfterThem)
{
	res::detail::watch_snapshot before;
	before.dirs = { "/c/old", "/c/old/inner" };
	before.files["/c/old/inner/x.cpp"] = state_of(1, 1);
	res::detail::watch_snapshot after;
	after.dirs = { "/c/fresh" };
	after.files["/c/fresh/y.cpp"] = state_of(2, 2);

	const auto batch = res::detail::diff_snapshots(before, after);
	std::vector<std::string> order;
	for (const auto& change : batch) {
		order.push_back(std::string{ change.kind == res::change_kind::created ? "+" : "-" } + change.path.generic_string());
	}
	const std::vector<std::string> expected{ "-/c/old/inner", "-/c/old", "-/c/old/inner/x.cpp", "+/c/fresh", "+/c/fresh/y.cpp" };
	BOOST_TEST(order == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ResCodeOps)

BOOST_FIXTURE_TEST_CASE(Remove_AFile_DeletesItAndTellsSubscribers, ops_fixture)
{
	const auto result = sys->remove(res::tag{ "code://src/game.cpp" });

	BOOST_TEST(result.ok);
	BOOST_TEST(!fs::exists(code / "src" / "game.cpp"));
	BOOST_TEST(!sys->exists(res::tag{ "code://src/game.cpp" }));
	BOOST_TEST(pump_until([this] { return has(res::change_kind::deleted, "code://src/game.cpp", true); }));
}

BOOST_FIXTURE_TEST_CASE(Remove_WhatIsNotCodeOrNotThere_IsRefused, ops_fixture)
{
	fs::create_directories(code / "build");
	dir.write("project/code/build/gen.cpp", "generated");

	BOOST_TEST(!sys->remove(res::tag{ "code://src/notes.md" }).ok);
	BOOST_TEST(!sys->remove(res::tag{ "code://src/missing.cpp" }).ok);
	BOOST_TEST(!sys->remove(res::tag{ "code://src" }).ok);
	BOOST_TEST(!sys->remove(res::tag{ "code://build/gen.cpp" }).ok);
	BOOST_TEST(!sys->remove(res::tag{ "code://../res/levels/main.desc" }).ok);
	BOOST_TEST(fs::exists(code / "src" / "notes.md"));
	BOOST_TEST(fs::exists(code / "src"));
	BOOST_TEST(fs::exists(code / "build" / "gen.cpp"));
	BOOST_TEST(fs::exists(content / "levels" / "main.desc"));
	BOOST_TEST(!sys->remove(res::tag{ "code://src/missing.cpp" }).error.empty());
}

BOOST_FIXTURE_TEST_CASE(Remove_ThroughALinkedFolder_IsRefused, ops_fixture)
{
	fs::create_directories(dir.path() / "outside");
	dir.write("outside/victim.cpp", "keep");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", code / "linked", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	BOOST_TEST(!sys->remove(res::tag{ "code://linked/victim.cpp" }).ok);
	BOOST_TEST(fs::exists(dir.path() / "outside" / "victim.cpp"));
}

BOOST_FIXTURE_TEST_CASE(Remove_InAnotherProtocol_IsRefused, ops_fixture)
{
	BOOST_TEST(!sys->remove(res::tag{ "res://levels/main.desc" }).ok);
	BOOST_TEST(fs::exists(content / "levels" / "main.desc"));
}

BOOST_FIXTURE_TEST_CASE(Rename_AFile_MovesItAndTellsSubscribers, ops_fixture)
{
	const auto result = sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "code://src/player.cpp" });

	BOOST_TEST(result.ok);
	BOOST_TEST(!fs::exists(code / "src" / "game.cpp"));
	BOOST_TEST(read_file(code / "src" / "player.cpp") == "int game;");
	BOOST_TEST(pump_until([this] { return has(res::change_kind::renamed, "code://src/player.cpp", true); }));
	const auto renamed = std::ranges::find_if(seen, [](const res::file_change& c) { return c.kind == res::change_kind::renamed; });
	BOOST_REQUIRE(renamed != seen.end());
	BOOST_TEST(renamed->old_tag.view() == "code://src/game.cpp");
}

BOOST_FIXTURE_TEST_CASE(Rename_IntoANewFolder_MakesTheFolder, ops_fixture)
{
	BOOST_TEST(sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "code://src/game/core/game.cpp" }).ok);
	BOOST_TEST(read_file(code / "src" / "game" / "core" / "game.cpp") == "int game;");
}

BOOST_FIXTURE_TEST_CASE(Rename_OverAnExistingFile_IsRefusedAndKeepsBoth, ops_fixture)
{
	dir.write("project/code/src/player.cpp", "int player;");

	const auto result = sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "code://src/player.cpp" });

	BOOST_TEST(!result.ok);
	BOOST_TEST(read_file(code / "src" / "game.cpp") == "int game;");
	BOOST_TEST(read_file(code / "src" / "player.cpp") == "int player;");
}

BOOST_FIXTURE_TEST_CASE(Rename_ToAnythingTheWritePolicyRefuses_IsRefused, ops_fixture)
{
	fs::create_directories(code / "build");

	BOOST_TEST(!sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "code://src/game.txt" }).ok);
	BOOST_TEST(!sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "code://build/game.cpp" }).ok);
	BOOST_TEST(!sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "code://../game.cpp" }).ok);
	BOOST_TEST(!sys->rename(res::tag{ "code://src/notes.md" }, res::tag{ "code://src/notes.cpp" }).ok);
	BOOST_TEST(!sys->rename(res::tag{ "code://src/game.cpp" }, res::tag{ "res://levels/game.cpp" }).ok);
	BOOST_TEST(fs::exists(code / "src" / "game.cpp"));
	BOOST_TEST(fs::exists(code / "src" / "notes.md"));
	BOOST_TEST(!fs::exists(content / "levels" / "game.cpp"));
}

BOOST_FIXTURE_TEST_CASE(Store_TellsSubscribersCreatedThenModified, ops_fixture)
{
	BOOST_TEST(sys->store_sync(res::tag{ "code://src/new.cpp" }, to_bytes("one")));
	BOOST_TEST(sys->store_sync(res::tag{ "code://src/new.cpp" }, to_bytes("two")));

	BOOST_TEST(pump_until([this] { return has(res::change_kind::created, "code://src/new.cpp", true)
		&& has(res::change_kind::modified, "code://src/new.cpp", true); }));
}

BOOST_FIXTURE_TEST_CASE(Watcher_FindsExternalChanges_AndSkipsWhatIsNotCode, ops_fixture)
{
	fs::create_directories(code / "build");
	dir.write("project/code/build/gen.cpp", "generated");
	dir.write("project/code/src/readme.md", "docs");
	dir.write("project/code/src/external.cpp", "int external;");

	BOOST_TEST(pump_until([this] { return has(res::change_kind::created, "code://src/external.cpp", false); }));
	BOOST_TEST(!has_tag("code://build/gen.cpp"));
	BOOST_TEST(!has_tag("code://src/readme.md"));

	dir.write("project/code/src/external.cpp", "int external; int more;");
	BOOST_TEST(pump_until([this] { return has(res::change_kind::modified, "code://src/external.cpp", false); }));

	fs::remove(code / "src" / "external.cpp");
	BOOST_TEST(pump_until([this] { return has(res::change_kind::deleted, "code://src/external.cpp", false); }));
}

BOOST_FIXTURE_TEST_CASE(Watcher_DoesNotRepeatOurOwnWritesAsExternal, ops_fixture)
{
	BOOST_TEST(sys->store_sync(res::tag{ "code://src/mine.cpp" }, to_bytes("mine")));
	dir.write("project/code/src/marker.cpp", "marker");

	BOOST_TEST(pump_until([this] { return has(res::change_kind::created, "code://src/marker.cpp", false); }));
	BOOST_TEST(!has(res::change_kind::created, "code://src/mine.cpp", false));
	BOOST_TEST(!has(res::change_kind::modified, "code://src/mine.cpp", false));
}

BOOST_FIXTURE_TEST_CASE(Watcher_ExternalRename_IsOneRenamedChange, ops_fixture)
{
	fs::rename(code / "src" / "game.cpp", code / "src" / "hero.cpp");

	BOOST_TEST(pump_until([this] { return has(res::change_kind::renamed, "code://src/hero.cpp", false); }));
}

BOOST_FIXTURE_TEST_CASE(Unsubscribe_StopsTheDelivery, ops_fixture)
{
	sys->unsubscribe_changes(&owner);

	BOOST_TEST(sys->remove(res::tag{ "code://src/game.cpp" }).ok);
	pump_until([this] { return !seen.empty(); }, 1500ms);
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_SUITE_END()
