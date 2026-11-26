#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/common/ds/ds_task_system.hpp>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_save_service.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// The save service (#1487, EPIC-66): a slot written transactionally under user://saves/, the previous bytes kept as
// <slot>.bak, the answer a value.
namespace {

namespace fs = std::filesystem;

std::vector<std::byte> bytes_of(std::string_view text)
{
	const auto* first = reinterpret_cast<const std::byte*>(text.data());
	return { first, first + text.size() };
}

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

std::vector<std::string> names_in(const fs::path& folder)
{
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto& entry : fs::directory_iterator(folder, ec)) names.push_back(entry.path().filename().string());
	std::sort(names.begin(), names.end());
	return names;
}

using names = std::vector<std::string>;

struct save_fixture : test_util::ram_res_env
{
	save_fixture()
	{
		BOOST_REQUIRE(rs->add_mount(res::mount{ res::tag::user, { dir.path() }, res::mount_access::read_write }));
	}

	res::save_result write(std::string_view slot, std::string_view text, res::save_options options = {})
	{
		auto handle = service.write(slot, bytes_of(text), std::move(options));
		pump();
		BOOST_REQUIRE(handle.status() == ds::task_status::ready);
		return handle.get();
	}

	fs::path saves() const { return dir.path() / "saves"; }

	test_util::temp_dir dir{ "res_save_service" };
	res::save_service service{ tasks, *rs };
};

}

BOOST_AUTO_TEST_SUITE(ResSaveService)

BOOST_AUTO_TEST_CASE(ASlotIsAPortableRelativeNameThatIsNotAFileOfTheServiceItself)
{
	for (const char* good : { "slot1", "profiles/a/slot_2", "auto save 3", "a.sav", "a-b_c.d" }) {
		BOOST_TEST_CONTEXT(good) { BOOST_TEST(res::save_service::slot_problem(good).empty()); }
	}
	for (const char* bad : { "", "../x", "a/../x", "/abs", "a//b", "a\\b", ".hidden", "dir/.hidden", ".dir/x", "x.bak", "dir/x.tmp", "x.corrupt",
			"x.BAK", "X.Bak", "x.TMP", "x.bak/y", "x.BAK/y/z", "C:x", "con", "nul.sav", "trailing.", "trailing ", "a<b", "a>b", "a\"b", "a|b",
			"a?b", "a*b", "bad\xFF", "bad\xC3", "over\xED\xA0\x80", "Save", "SLOT", "profiles/Slot", "caf\xC3\xA9", "caf\xC3\x89", "slot~1", "a@b" }) {
		BOOST_TEST_CONTEXT(bad) { BOOST_TEST(!res::save_service::slot_problem(bad).empty()); }
	}
	BOOST_TEST(!res::save_service::slot_problem(std::string(res::save_service::k_max_slot_bytes + 1, 'a')).empty());
	BOOST_TEST(!res::save_service::slot_problem(std::string(res::save_service::k_max_name_bytes + 1, 'a')).empty());
	BOOST_TEST(res::save_service::slot_problem(std::string(res::save_service::k_max_name_bytes, 'a')).empty());
}

BOOST_FIXTURE_TEST_CASE(TheFirstWriteMakesTheFile_WithoutABackup_AndLeavesNoTempFile, save_fixture)
{
	const res::save_result result = write("slot1", "one");
	BOOST_TEST(result.status == res::save_status::ok);
	BOOST_TEST(result.error.empty());
	BOOST_TEST(result.bytes == 3u);
	BOOST_TEST(!result.backup_made);
	BOOST_TEST(result.backup_error.empty());
	BOOST_TEST(read_file(saves() / "slot1") == "one");
	BOOST_TEST(names_in(saves()) == names{ "slot1" }, boost::test_tools::per_element());
}

BOOST_FIXTURE_TEST_CASE(EachWriteKeepsThePreviousBytesAsTheBackup, save_fixture)
{
	BOOST_REQUIRE(write("profiles/p1/slot", "one").status == res::save_status::ok);
	const res::save_result second = write("profiles/p1/slot", "two");
	BOOST_TEST(second.status == res::save_status::ok);
	BOOST_TEST(second.backup_made);
	BOOST_TEST(read_file(saves() / "profiles" / "p1" / "slot") == "two");
	BOOST_TEST(read_file(saves() / "profiles" / "p1" / "slot.bak") == "one");

	BOOST_REQUIRE(write("profiles/p1/slot", "three").status == res::save_status::ok);
	BOOST_TEST(read_file(saves() / "profiles" / "p1" / "slot.bak") == "two");
	BOOST_TEST(names_in(saves() / "profiles" / "p1") == (names{ "slot", "slot.bak" }), boost::test_tools::per_element());
}

BOOST_FIXTURE_TEST_CASE(AFileThatIsEmptyOrNotValidIsNeverMadeTheBackup_SoTheLastGoodOneStays, save_fixture)
{
	BOOST_REQUIRE(write("slot", "good").status == res::save_status::ok);
	BOOST_REQUIRE(write("slot", "second").status == res::save_status::ok);
	BOOST_TEST(read_file(saves() / "slot.bak") == "good");

	std::ofstream(saves() / "slot", std::ios::binary | std::ios::trunc).close();
	const res::save_result after_empty = write("slot", "third");
	BOOST_TEST(after_empty.status == res::save_status::ok);
	BOOST_TEST(!after_empty.backup_made);
	BOOST_TEST(read_file(saves() / "slot") == "third");
	BOOST_TEST(read_file(saves() / "slot.bak") == "good");

	res::save_options judge;
	judge.is_valid = [](std::span<const std::byte> bytes) { return bytes.size() != 6u; };
	BOOST_TEST(write("slot", "fourth", judge).backup_made);
	BOOST_TEST(read_file(saves() / "slot.bak") == "third");
	BOOST_TEST(!write("slot", "fifth", judge).backup_made);
	BOOST_TEST(read_file(saves() / "slot.bak") == "third");

	res::save_options throwing;
	throwing.is_valid = [](std::span<const std::byte>) -> bool { throw std::runtime_error("bad"); };
	BOOST_TEST(!write("slot", "sixth", throwing).backup_made);
	BOOST_TEST(read_file(saves() / "slot") == "sixth");
	BOOST_TEST(read_file(saves() / "slot.bak") == "third");
}

BOOST_FIXTURE_TEST_CASE(AWriteThatStopsAtAnyStage_LeavesTheSlotOrItsBackupWhole_AndTheBackupIsNeverTheNewestCopy, save_fixture)
{
	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	BOOST_REQUIRE(write("slot", "v2").status == res::save_status::ok);
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");

	const auto stop_at = [this](res::save_stage stage) {
		service.set_stage_hook_for_tests([stage](res::save_stage at) {
			if (at == stage) throw std::runtime_error("power cut");
		});
		const res::save_result result = write("slot", "v3");
		service.set_stage_hook_for_tests({});
		return result;
	};

	BOOST_TEST(stop_at(res::save_stage::before_read).status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");

	BOOST_TEST(stop_at(res::save_stage::before_replace).status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");

	// The new bytes are in place, the backup is not made yet: it still holds the generation before, not a copy of the
	// file the slot held until now.
	BOOST_TEST(stop_at(res::save_stage::after_replace).status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v3");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");
	BOOST_TEST(names_in(saves()) == (names{ "slot", "slot.bak" }), boost::test_tools::per_element());

	BOOST_TEST(write("slot", "v4").backup_made);
	BOOST_TEST(read_file(saves() / "slot.bak") == "v3");
}

BOOST_FIXTURE_TEST_CASE(AWriteThatStopsBetweenItsTempFileAndTheRename_LeavesTheOldFileAndNoTempFile, save_fixture)
{
	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	BOOST_REQUIRE(write("slot", "v2").status == res::save_status::ok);

	// The slot's own file: the temp file is written and flushed, then the "crash".
	service.set_write_hook_for_tests([](platform::beneath::write_stage stage, const fs::path&) {
		if (stage == platform::beneath::write_stage::before_rename) throw std::runtime_error("power cut");
	});
	const res::save_result stopped = write("slot", "v3");
	BOOST_TEST(stopped.status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");
	BOOST_TEST(names_in(saves()) == (names{ "slot", "slot.bak" }), boost::test_tools::per_element());

	// The backup's file: the slot is the new one by then, the backup is the one before.
	service.set_write_hook_for_tests([](platform::beneath::write_stage stage, const fs::path& tmp) {
		if (stage == platform::beneath::write_stage::before_rename && tmp.filename().string().find(".bak.") != std::string::npos) {
			throw std::runtime_error("power cut");
		}
	});
	const res::save_result half = write("slot", "v3");
	BOOST_TEST(half.status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v3");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");
	BOOST_TEST(names_in(saves()) == (names{ "slot", "slot.bak" }), boost::test_tools::per_element());
	service.set_write_hook_for_tests({});
}

BOOST_FIXTURE_TEST_CASE(ABackupThatCannotBeMade_DoesNotUndoTheSave_AndIsToldAsAValue, save_fixture)
{
	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	fs::create_directories(saves() / "slot.bak");
	const res::save_result result = write("slot", "v2");
	BOOST_TEST(result.status == res::save_status::ok);
	BOOST_TEST(!result.backup_made);
	BOOST_TEST(!result.backup_error.empty());
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(fs::is_directory(saves() / "slot.bak"));
}

BOOST_FIXTURE_TEST_CASE(ARefusedWriteIsAValue_AndWritesNothing, save_fixture)
{
	const res::save_result bad_slot = write("../outside", "x");
	BOOST_TEST(bad_slot.status == res::save_status::refused);
	BOOST_TEST(!bad_slot.error.empty());
	BOOST_TEST(!fs::exists(dir.path() / "outside"));
	BOOST_TEST(!fs::exists(saves()));

	BOOST_TEST(write("x.BAK", "x").status == res::save_status::refused);
	BOOST_TEST(write("x.bak/y", "x").status == res::save_status::refused);
	BOOST_TEST(!fs::exists(saves()));

	auto too_big = service.write("slot", std::vector<std::byte>(res::save_service::k_max_bytes + 1));
	pump();
	BOOST_REQUIRE(too_big.status() == ds::task_status::ready);
	BOOST_TEST(too_big.get().status == res::save_status::too_large);
	BOOST_TEST(!fs::exists(saves()));

	fs::create_directories(saves() / "slot");
	const res::save_result over_a_folder = write("slot", "x");
	BOOST_TEST(over_a_folder.status == res::save_status::refused);
	BOOST_TEST(fs::is_directory(saves() / "slot"));
}

BOOST_FIXTURE_TEST_CASE(ALinkAtTheSlotOrItsBackup_IsNeverFollowed, save_fixture)
{
	fs::create_directories(saves());
	const fs::path outside = dir.path() / "outside.txt";
	std::ofstream(outside, std::ios::binary) << "keep";

	std::error_code ec;
	fs::create_symlink(outside, saves() / "linked", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	BOOST_TEST(write("linked", "x").status == res::save_status::refused);
	BOOST_TEST(read_file(outside) == "keep");
	BOOST_TEST(fs::is_symlink(saves() / "linked"));

	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	fs::create_symlink(outside, saves() / "slot.bak");
	BOOST_REQUIRE(write("slot", "v2").status == res::save_status::ok);
	BOOST_TEST(read_file(outside) == "keep");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");
	BOOST_TEST(!fs::is_symlink(saves() / "slot.bak"));

	fs::remove_all(saves());
	fs::create_directories(dir.path() / "elsewhere");
	fs::create_directory_symlink(dir.path() / "elsewhere", saves(), ec);
	BOOST_REQUIRE(!ec);
	const res::save_result through_a_link = write("slot", "x");
	BOOST_TEST(through_a_link.status != res::save_status::ok);
	BOOST_TEST(fs::is_empty(dir.path() / "elsewhere"));
}

BOOST_FIXTURE_TEST_CASE(AFileAtTheSlotThatCannotBeKept_DoesNotMakeTheSlotUnwritable, save_fixture)
{
	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	BOOST_REQUIRE(write("slot", "v2").status == res::save_status::ok);
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");

	// Over the size a save may have: nobody can read it as a save, so it is not kept as the backup and is replaced.
	std::error_code ec;
	fs::resize_file(saves() / "slot", res::save_service::k_max_bytes + 1, ec);
	if (ec) {
		BOOST_TEST_MESSAGE("no room for a big file: " << ec.message());
		return;
	}
	const res::save_result result = write("slot", "v3");
	BOOST_TEST(result.status == res::save_status::ok);
	BOOST_TEST(!result.backup_made);
	BOOST_TEST(read_file(saves() / "slot") == "v3");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");
}

BOOST_FIXTURE_TEST_CASE(TheTempFilesACrashLeft_AreRemovedTheFirstTimeAFolderIsWritten_AndNothingElse, save_fixture)
{
	fs::create_directories(saves());
	const std::string stale = ".slot.0123456789abcdef.tmp";
	std::ofstream(saves() / stale, std::ios::binary) << "half";
	std::ofstream(saves() / ".keep", std::ios::binary) << "mine";
	std::ofstream(saves() / "other.tmp", std::ios::binary) << "mine";
	std::ofstream(saves() / ".notours.tmp", std::ios::binary) << "mine";

	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	BOOST_TEST(names_in(saves()) == (names{ ".keep", ".notours.tmp", "other.tmp", "slot" }), boost::test_tools::per_element());
}

BOOST_FIXTURE_TEST_CASE(TheSweepOfTempFiles_LeavesFoldersLinksAndWhatIsInSubfolders, save_fixture)
{
	fs::create_directories(saves() / "sub");
	const std::string stale = ".slot.0123456789abcdef.tmp";
	std::ofstream(saves() / stale, std::ios::binary) << "half";
	std::ofstream(saves() / "sub" / stale, std::ios::binary) << "of a folder not written";
	fs::create_directories(saves() / ".dir.0123456789abcdef.tmp");
	std::ofstream(saves() / ".dir.0123456789abcdef.tmp" / "inner", std::ios::binary) << "mine";
	std::error_code ec;
	fs::create_symlink(dir.path() / "nowhere", saves() / ".link.0123456789abcdef.tmp", ec);
	const bool linked = !ec;

	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	BOOST_TEST(!fs::exists(saves() / stale));
	BOOST_TEST(fs::exists(saves() / "sub" / stale));
	BOOST_TEST(fs::is_directory(saves() / ".dir.0123456789abcdef.tmp"));
	BOOST_TEST(fs::exists(saves() / ".dir.0123456789abcdef.tmp" / "inner"));
	if (linked) BOOST_TEST(fs::is_symlink(saves() / ".link.0123456789abcdef.tmp"));
}

#ifndef _WIN32
BOOST_FIXTURE_TEST_CASE(AFileAtTheSlotThatCannotBeRead_IsReplacedAndNotKeptAsTheBackup, save_fixture)
{
	BOOST_REQUIRE(write("slot", "v1").status == res::save_status::ok);
	BOOST_REQUIRE(write("slot", "v2").status == res::save_status::ok);
	fs::permissions(saves() / "slot", fs::perms::none);
	if (std::ifstream(saves() / "slot", std::ios::binary).good()) {
		BOOST_TEST_MESSAGE("the file is readable whatever its mode (running as root): not testable here");
		return;
	}
	const res::save_result result = write("slot", "v3");
	BOOST_TEST(result.status == res::save_status::ok);
	BOOST_TEST(!result.backup_made);
	BOOST_TEST(read_file(saves() / "slot") == "v3");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");
}
#endif

BOOST_AUTO_TEST_CASE(WithoutUserMounted_AWriteIsRefusedAsAValue)
{
	test_util::ram_res_env env;
	res::save_service service{ env.tasks, *env.rs };
	auto handle = service.write("slot", bytes_of("x"));
	env.pump();
	BOOST_REQUIRE(handle.status() == ds::task_status::ready);
	BOOST_TEST(handle.get().status == res::save_status::refused);
}

BOOST_FIXTURE_TEST_CASE(AWriteCancelledBeforeItStartedWritesNothing_AndGivesItsShareOfTheQueueBack, save_fixture)
{
	service.set_queue_limit_for_tests(4);
	auto cancelled = service.write("slot", bytes_of("abcd"));
	cancelled.cancel();
	pump();
	BOOST_TEST(cancelled.status() == ds::task_status::cancelled);
	BOOST_TEST(!fs::exists(saves() / "slot"));
	BOOST_TEST(write("slot", "abcd").status == res::save_status::ok);
}

BOOST_FIXTURE_TEST_CASE(TooManyBytesWaitingToBeWritten_IsBusy_UntilTheyAreWritten, save_fixture)
{
	service.set_queue_limit_for_tests(10);
	auto first = service.write("one", bytes_of("123456"));
	auto second = service.write("two", bytes_of("123456"));
	pump();
	BOOST_REQUIRE(first.status() == ds::task_status::ready);
	BOOST_REQUIRE(second.status() == ds::task_status::ready);
	BOOST_TEST(first.get().status == res::save_status::ok);
	BOOST_TEST(second.get().status == res::save_status::busy);
	BOOST_TEST(!fs::exists(saves() / "two"));
	BOOST_TEST(write("two", "123456").status == res::save_status::ok);
}

BOOST_AUTO_TEST_CASE(TheServiceWaitsForTheWritesQueuedWhenItGoes)
{
	test_util::temp_dir dir{ "res_save_service_wait" };
	test_util::ram_res_env env;
	BOOST_REQUIRE(env.rs->add_mount(res::mount{ res::tag::user, { dir.path() }, res::mount_access::read_write }));
	{
		res::save_service service{ env.tasks, *env.rs };
		[[maybe_unused]] auto handle = service.write("slot", bytes_of("queued"));
	}
	BOOST_TEST(read_file(dir.path() / "saves" / "slot") == "queued");
}

BOOST_FIXTURE_TEST_CASE(AWriteBeforeUserIsServedFromOtherFolders_IsFlushedFirst_AndNothingIsLostToTheNewFolder, save_fixture)
{
	test_util::temp_dir other{ "res_save_service_other" };

	auto before = service.write("slot", bytes_of("profile A"));
	service.flush();
	BOOST_TEST(before.status() == ds::task_status::ready);
	BOOST_TEST(read_file(saves() / "slot") == "profile A");

	BOOST_REQUIRE(rs->remount(res::tag::user, { other.path() }));
	BOOST_TEST(write("slot", "profile B").status == res::save_status::ok);
	BOOST_TEST(read_file(other.path() / "saves" / "slot") == "profile B");
	BOOST_TEST(read_file(saves() / "slot") == "profile A");
	BOOST_TEST(!fs::exists(other.path() / "saves" / "slot.bak"));
}

BOOST_FIXTURE_TEST_CASE(TheSameSlotInTwoUserFolders_IsTwoSlots_ForTheOrderOfWrites, save_fixture)
{
	test_util::temp_dir other{ "res_save_service_other_order" };

	// Queued under the first folder, the folder swapped, queued under the second: the older one is not "superseded" by
	// the newer one of another folder.
	auto in_first = service.write("slot", bytes_of("first folder"));
	BOOST_REQUIRE(rs->remount(res::tag::user, { other.path() }));
	auto in_second = service.write("slot", bytes_of("second folder"));
	pump();
	BOOST_REQUIRE(in_first.status() == ds::task_status::ready);
	BOOST_REQUIRE(in_second.status() == ds::task_status::ready);
	BOOST_TEST(in_first.get().status == res::save_status::ok);
	BOOST_TEST(in_second.get().status == res::save_status::ok);
	BOOST_TEST(read_file(saves() / "slot") == "first folder");
	BOOST_TEST(read_file(other.path() / "saves" / "slot") == "second folder");
}

// Writers on two io threads: the last call of a slot is the one that stays, whatever order the threads take them in.
BOOST_AUTO_TEST_CASE(WritesOfOneSlotFromThreads_LeaveTheNewestOne_AndTheOlderOnesAreOkOrSuperseded)
{
	test_util::temp_dir dir{ "res_save_service_threads" };
	ds::task_system_config config;
	config.mode = ds::policy::threaded;
	config.io_threads = 2;
	ds::task_system tasks{ config };
	std::filesystem::create_directories("./res");
	std::filesystem::create_directories("./sandbox");
	res::resource_system rs{ tasks };
	BOOST_REQUIRE(rs.add_mount(res::mount{ res::tag::user, { dir.path() }, res::mount_access::read_write }));
	res::save_service service{ tasks, rs };

	constexpr int k_writes = 60;
	std::vector<ds::task_handle<res::save_result>> handles;
	for (int i = 0; i < k_writes; ++i) handles.push_back(service.write("slot", bytes_of("v" + std::to_string(i))));
	service.flush();
	for (const auto& handle : handles) {
		BOOST_REQUIRE(handle.status() == ds::task_status::ready);
		const res::save_status status = handle.get().status;
		BOOST_TEST((status == res::save_status::ok || status == res::save_status::superseded));
	}
	BOOST_TEST(handles.back().get().status == res::save_status::ok);
	BOOST_TEST(read_file(dir.path() / "saves" / "slot") == "v" + std::to_string(k_writes - 1));
	const names left = names_in(dir.path() / "saves");
	BOOST_TEST(std::ranges::all_of(left, [](const std::string& name) { return name == "slot" || name == "slot.bak"; }));
}

BOOST_AUTO_TEST_SUITE_END()
