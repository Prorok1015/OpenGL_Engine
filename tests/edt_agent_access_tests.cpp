#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_access.h"
#include "edt_mcp_bridge_host.h"
#include <snk/gui/gui_ui_font.h>
#include <snk/command/cmd_main_thread_queue.h>
#include <snk/command/cmd_registry.h>
#include <imgui.h>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The pieces behind an agent's `ask` wired together (A6b #293, PR-4a): the gate and its teardown, the font check, and the
// end of an MCP session that declines what it still waits on.
namespace {

using cmd::agent_category;

struct access_fixture
{
	test_util::temp_dir dir{ "edt_agent_access" };
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	cmd::main_thread_queue queue{ commands, tasks };
	std::unique_ptr<edt::agent_access> access;
	std::unique_ptr<edt::mcp_bridge_host> bridge;
	std::vector<cmd::registration> registrations;
	std::vector<cmd::result> finished;
	int runs = 0;

	access_fixture()
	{
		cmd::command_desc desc;
		desc.id = "code.create";
		desc.category = agent_category::code_files;
		desc.summary = "creates a file";
		desc.args_schema = boost::json::value{ { "type", "object" } };
		desc.invoke = [this](const cmd::call&) {
			++runs;
			return cmd::result::success();
		};
		registrations.push_back(commands.add(std::move(desc)));
		access = std::make_unique<edt::agent_access>(commands, dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json");
		bridge = std::make_unique<edt::mcp_bridge_host>(commands, queue, tasks, access->confirmations());
	}

	~access_fixture()
	{
		bridge.reset();
		access.reset();
	}

	void ask_as(std::string session)
	{
		cmd::call request;
		request.args = boost::json::object{};
		request.from = cmd::origin::agent(std::move(session));
		access->confirmations().invoke("code.create", std::move(request), [this](cmd::result outcome) { finished.push_back(std::move(outcome)); });
	}
};

std::vector<std::byte> read_bytes(const std::filesystem::path& file)
{
	std::ifstream in(file, std::ios::binary);
	std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	std::vector<std::byte> bytes(chars.size());
	for (std::size_t i = 0; i < chars.size(); ++i) {
		bytes[i] = static_cast<std::byte>(chars[i]);
	}
	return bytes;
}

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentAccess, access_fixture)

BOOST_AUTO_TEST_CASE(NobodyToAsk_AnAskIsADeny)
{
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(access->confirmations().pending().empty());
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(FrameWithoutAFont_KeepsNobodyToAsk)
{
	BOOST_TEST(access->frame(nullptr));
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
}

BOOST_AUTO_TEST_CASE(PersonPresent_AnAskWaitsForTheAnswer)
{
	access->set_person_present(true);
	ask_as("s1");

	BOOST_TEST(finished.empty());
	BOOST_REQUIRE(access->confirmations().pending().size() == 1u);
}

BOOST_AUTO_TEST_CASE(SessionClosed_DeclinesOnlyThatSessionsRequests)
{
	access->set_person_present(true);
	ask_as("s1");
	ask_as("s2");

	bridge->session_closed("s1");
	BOOST_TEST(access->confirmations().pending().size() == 2u);
	tasks.drain_main(std::chrono::hours(1));

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	const auto left = access->confirmations().pending();
	BOOST_REQUIRE(left.size() == 1u);
	BOOST_TEST(left[0].session == "s2");
}

BOOST_AUTO_TEST_CASE(SessionClosed_ForgetsTheSessionsGrants)
{
	access->set_person_present(true);
	ask_as("s1");
	access->confirmations().answer(access->confirmations().pending().front().id, edt::confirmation_answer::allow_session);
	BOOST_TEST(access->grants().has("s1", agent_category::code_files));

	bridge->session_closed("s1");
	tasks.drain_main(std::chrono::hours(1));

	BOOST_TEST(!access->grants().has("s1", agent_category::code_files));
}

BOOST_AUTO_TEST_CASE(DestroyingTheAccess_AnswersWaitingRequestsAndRemovesTheGate)
{
	access->set_person_present(true);
	ask_as("s1");
	bridge.reset();
	access.reset();

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	cmd::call request;
	request.args = boost::json::object{};
	request.from = cmd::origin::agent("s1");
	BOOST_TEST(commands.invoke("code.create", request).ok);
}

BOOST_AUTO_TEST_CASE(SessionClosedAfterTheBridgeIsGone_IsHarmless)
{
	bridge->session_closed("s1");
	bridge.reset();

	BOOST_CHECK_NO_THROW(tasks.drain_main(std::chrono::hours(1)));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtConfirmationGlyphs)

BOOST_AUTO_TEST_CASE(TheShippedFont_DrawsEveryButtonLetter)
{
	gui::ui_font font;
	font.set_bytes(read_bytes(std::filesystem::path{ SNK_CORE_RES_DIR } / "fonts" / "NotoSans-Regular.ttf"));
	BOOST_REQUIRE(font.usable());
	ImFontAtlas atlas;
	ImFont* added = font.add_to(atlas, 13.f);
	BOOST_REQUIRE(added != nullptr);
	BOOST_REQUIRE(atlas.Build());

	BOOST_TEST(edt::confirmation_glyphs_available(*added));
}

BOOST_AUTO_TEST_CASE(TheBuiltInFont_HasNoCyrillic)
{
	ImFontAtlas atlas;
	ImFont* added = atlas.AddFontDefault();
	BOOST_REQUIRE(added != nullptr);
	BOOST_REQUIRE(atlas.Build());

	BOOST_TEST(!edt::confirmation_glyphs_available(*added));
}

BOOST_AUTO_TEST_SUITE_END()
