#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include "edt_build_settings.h"
#include <snk/mcp/mcp_connection_file.h>
#include "test_temp_dir.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#ifndef _WIN32
#include <sys/stat.h>
#endif

// The project's .snk files are read without following a link at .snk or at the file (#526): the policy fails closed,
// build.json is broken (defaults), mcp.json is not there.

namespace {

namespace fs = std::filesystem;
using cmd::agent_category;
using cmd::policy_mode;

cmd::command_desc command_of(std::string id, agent_category category)
{
	cmd::command_desc desc;
	desc.id = std::move(id);
	desc.category = category;
	desc.summary = "does " + desc.id;
	desc.invoke = [](const cmd::call&) { return cmd::result::success(); };
	return desc;
}

// `link` becomes a link to the folder `target`: a symlink on POSIX, a junction on Windows (no privilege needed).
void make_folder_link(const fs::path& target, const fs::path& link)
{
#ifdef _WIN32
	const std::string command = "cmd /c mklink /J \"" + link.string() + "\" \"" + target.string() + "\" >nul";
	BOOST_REQUIRE_MESSAGE(std::system(command.c_str()) == 0 && fs::exists(link), "mklink /J failed for " + link.string());
#else
	fs::create_directory_symlink(target, link);
#endif
}

// False (with the reason in the test log) when Windows refuses a file symlink to this user.
bool make_file_link(const fs::path& target, const fs::path& link)
{
	std::error_code ec;
	fs::create_symlink(target, link, ec);
#ifdef _WIN32
	if (ec) {
		BOOST_TEST_MESSAGE("file symlink not created (" + ec.message() + "): no developer mode or symlink privilege; that case is not run");
		return false;
	}
#endif
	BOOST_REQUIRE_MESSAGE(!ec, "cannot create a symlink: " + ec.message());
	return true;
}

constexpr std::string_view k_open_policy = R"json({"categories":{"scene_edit":"allow","code_files":"allow","build":"allow"}})json";
constexpr std::string_view k_live_settings = R"json({"live_enabled": true, "jobs_mode": "manual"})json";

}

BOOST_AUTO_TEST_SUITE(EdtNoFollowReadsTests)

BOOST_AUTO_TEST_CASE(Policy_ALinkAtTheProjectFile_FailsClosed)
{
	test_util::temp_dir dir("edt_nofollow_policy_file_link");
	const fs::path victim = dir.write("victim.json", k_open_policy);
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(victim, dir.path() / ".snk" / "agent_policy.json")) return;

	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}

BOOST_AUTO_TEST_CASE(Policy_ADanglingLink_IsALayerError_NotNoFile)
{
	test_util::temp_dir dir("edt_nofollow_policy_dangling");
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(dir.path() / "nowhere.json", dir.path() / ".snk" / "agent_policy.json")) return;

	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}

BOOST_AUTO_TEST_CASE(Policy_ALinkAtSnk_FailsClosed)
{
	test_util::temp_dir dir("edt_nofollow_policy_folder_link");
	dir.write("outside/agent_policy.json", k_open_policy);
	make_folder_link(dir.path() / "outside", dir.path() / ".snk");

	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}

BOOST_AUTO_TEST_CASE(Policy_APlainFile_IsStillRead)
{
	test_util::temp_dir dir("edt_nofollow_policy_plain");
	dir.write(".snk/agent_policy.json", "{}");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::allow));
	BOOST_TEST(policy.project_error().empty());
}

BOOST_AUTO_TEST_CASE(BuildSettings_ALinkAtTheFile_IsBroken_DefaultsAreUsed)
{
	test_util::temp_dir dir("edt_nofollow_build_file_link");
	const fs::path victim = dir.write("victim.json", k_live_settings);
	fs::create_directories(dir.path() / ".snk");
	if (!make_file_link(victim, dir.path() / ".snk" / "build.json")) return;

	BOOST_TEST(!edt::load_build_settings(dir.path() / ".snk").live_enabled);
	BOOST_TEST(edt::check_build_settings_file(dir.path() / ".snk").has_value());
}

BOOST_AUTO_TEST_CASE(BuildSettings_ALinkAtSnk_IsBroken_DefaultsAreUsed)
{
	test_util::temp_dir dir("edt_nofollow_build_folder_link");
	dir.write("outside/build.json", k_live_settings);
	make_folder_link(dir.path() / "outside", dir.path() / ".snk");

	BOOST_TEST(!edt::load_build_settings(dir.path() / ".snk").live_enabled);
	BOOST_TEST(edt::check_build_settings_file(dir.path() / ".snk").has_value());
}

BOOST_AUTO_TEST_CASE(BuildSettings_APlainFile_IsStillRead)
{
	test_util::temp_dir dir("edt_nofollow_build_plain");
	dir.write(".snk/build.json", k_live_settings);
	BOOST_TEST(edt::load_build_settings(dir.path() / ".snk").live_enabled);
	BOOST_TEST(!edt::check_build_settings_file(dir.path() / ".snk").has_value());
}

BOOST_AUTO_TEST_CASE(McpJson_BehindALinkAtSnk_IsNotThere)
{
	test_util::temp_dir dir("edt_nofollow_mcp_folder_link");
	dir.write("outside/mcp.json", R"json({"url":"http://127.0.0.1:7/mcp","port":7,"token":"linked","pid":1})json");
	BOOST_REQUIRE(mcp::read_connection_file(dir.path() / "outside").has_value());
	make_folder_link(dir.path() / "outside", dir.path() / ".snk");

	BOOST_TEST(!mcp::read_connection_file(dir.path() / ".snk").has_value());
}

BOOST_AUTO_TEST_CASE(McpJson_RemoveBehindALinkAtSnk_SaysWhy_AndDeletesNothing)
{
	test_util::temp_dir dir("edt_nofollow_mcp_remove_link");
	mcp::connection_info mine;
	mine.port = 7;
	mine.token = "mine";
	mine.pid = mcp::current_process_id();
	BOOST_REQUIRE(mcp::write_connection_file(dir.path() / "outside", mine).empty());
	make_folder_link(dir.path() / "outside", dir.path() / ".snk");

	BOOST_TEST(!mcp::remove_connection_file(dir.path() / ".snk", mine).empty());
	BOOST_TEST(fs::exists(mcp::connection_file_path(dir.path() / "outside")));
	BOOST_TEST(mcp::remove_connection_file(dir.path() / "outside", mine).empty());
	BOOST_TEST(!fs::exists(mcp::connection_file_path(dir.path() / "outside")));
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(Policy_ADanglingLinkAtSnk_FailsClosed)
{
	test_util::temp_dir dir("edt_nofollow_policy_dangling_folder");
	fs::create_directory_symlink(dir.path() / "nowhere", dir.path() / ".snk");

	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}

// A FIFO at the name is refused without blocking: the policy fails closed, build.json is broken.
BOOST_AUTO_TEST_CASE(Policy_AFifo_FailsClosed_AndReturns)
{
	test_util::temp_dir dir("edt_nofollow_policy_fifo");
	fs::create_directories(dir.path() / ".snk");
	BOOST_REQUIRE(::mkfifo((dir.path() / ".snk" / "agent_policy.json").c_str(), 0600) == 0);

	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}

BOOST_AUTO_TEST_CASE(BuildSettings_AFifo_IsBroken_AndReturns)
{
	test_util::temp_dir dir("edt_nofollow_build_fifo");
	fs::create_directories(dir.path() / ".snk");
	BOOST_REQUIRE(::mkfifo((dir.path() / ".snk" / "build.json").c_str(), 0600) == 0);

	BOOST_TEST(!edt::load_build_settings(dir.path() / ".snk").live_enabled);
	BOOST_TEST(edt::check_build_settings_file(dir.path() / ".snk").has_value());
	BOOST_TEST(edt::update_build_settings(dir.path() / ".snk", [](edt::build_settings& settings) {
		settings.live_enabled = true;
	}).has_value());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
