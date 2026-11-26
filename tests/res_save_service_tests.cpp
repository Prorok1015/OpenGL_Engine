#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_save_service.h>
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
	for (const char* good : { "slot1", "profiles/a/slot_2", "auto save 3", "a.sav" }) {
		BOOST_TEST_CONTEXT(good) { BOOST_TEST(res::save_service::slot_problem(good).empty()); }
	}
	for (const char* bad : { "", "../x", "a/../x", "/abs", "a//b", "a\\b", ".hidden", "dir/.hidden", "x.bak", "dir/x.tmp", "x.corrupt",
			"C:x", "con", "nul.sav", "trailing.", "trailing " }) {
		BOOST_TEST_CONTEXT(bad) { BOOST_TEST(!res::save_service::slot_problem(bad).empty()); }
	}
	BOOST_TEST(!res::save_service::slot_problem(std::string(res::save_service::k_max_slot_bytes + 1, 'a')).empty());
	BOOST_TEST(!res::save_service::slot_problem(std::string(101, 'a')).empty());
}

BOOST_FIXTURE_TEST_CASE(TheFirstWriteMakesTheFile_WithoutABackup_AndLeavesNoTempFile, save_fixture)
{
	const res::save_result result = write("slot1", "one");
	BOOST_TEST(result.status == res::save_status::ok);
	BOOST_TEST(result.error.empty());
	BOOST_TEST(result.bytes == 3u);
	BOOST_TEST(!result.backup_made);
	BOOST_TEST(read_file(saves() / "slot1") == "one");
	BOOST_TEST(names_in(saves()) == std::vector<std::string>{ "slot1" }, boost::test_tools::per_element());
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
	BOOST_TEST(names_in(saves() / "profiles" / "p1") == (std::vector<std::string>{ "slot", "slot.bak" }), boost::test_tools::per_element());
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

BOOST_FIXTURE_TEST_CASE(AWriteThatStopsAtAnyStage_LeavesTheSlotOrItsBackupWhole, save_fixture)
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

	BOOST_TEST(stop_at(res::save_stage::before_backup).status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v1");

	BOOST_TEST(stop_at(res::save_stage::after_backup).status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v2");

	BOOST_TEST(stop_at(res::save_stage::before_replace).status == res::save_status::io_error);
	BOOST_TEST(read_file(saves() / "slot") == "v2");
	BOOST_TEST(names_in(saves()) == (std::vector<std::string>{ "slot", "slot.bak" }), boost::test_tools::per_element());

	const res::save_result done = write("slot", "v3");
	BOOST_TEST(done.status == res::save_status::ok);
	BOOST_TEST(read_file(saves() / "slot") == "v3");
	BOOST_TEST(read_file(saves() / "slot.bak") == "v2");
}

BOOST_FIXTURE_TEST_CASE(ARefusedWriteIsAValue_AndWritesNothing, save_fixture)
{
	const res::save_result bad_slot = write("../outside", "x");
	BOOST_TEST(bad_slot.status == res::save_status::refused);
	BOOST_TEST(!bad_slot.error.empty());
	BOOST_TEST(!fs::exists(dir.path() / "outside"));
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

BOOST_AUTO_TEST_CASE(WithoutUserMounted_AWriteIsRefusedAsAValue)
{
	test_util::ram_res_env env;
	res::save_service service{ env.tasks, *env.rs };
	auto handle = service.write("slot", bytes_of("x"));
	env.pump();
	BOOST_REQUIRE(handle.status() == ds::task_status::ready);
	BOOST_TEST(handle.get().status == res::save_status::refused);
}

BOOST_FIXTURE_TEST_CASE(AWriteCancelledBeforeItStartedWritesNothing, save_fixture)
{
	auto handle = service.write("slot", bytes_of("x"));
	handle.cancel();
	pump();
	BOOST_TEST(handle.status() == ds::task_status::cancelled);
	BOOST_TEST(!fs::exists(saves() / "slot"));
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

BOOST_AUTO_TEST_SUITE_END()
