#include <boost/test/unit_test.hpp>
#include <snk/gui/gui_theme.h>
#include "test_manual_tasks.hpp"
#include "test_agent_confirmations.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_access.h"
#include "edt_agent_log_filter.h"
#include "edt_mcp_bridge_host.h"
#include <snk/gui/gui_ui_font.h>
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/logger/engine_log.h>
#include <imgui.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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
	std::chrono::steady_clock::time_point current{};
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
		access = std::make_unique<edt::agent_access>(commands, dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json",
			[this] { return current; });
		// The way editor_system wires the end of a session: the confirmations drop what they keep for it.
		bridge = std::make_unique<edt::mcp_bridge_host>(commands, tasks, *access,
			[this](const std::string& session) { access->confirmations().drop_session(session); });
	}

	~access_fixture()
	{
		if (access) access->confirmations().cancel_all("the test ends");
		bridge.reset();
		access.reset();
	}

	// A project that is its own git worktree, so the --agent-policy file next to it is outside both.
	std::filesystem::path project_root() const
	{
		std::filesystem::create_directories(dir.path() / "project" / ".git");
		return dir.path() / "project";
	}

	void person_arrives()
	{
		access->set_questions_routed(true);
		access->set_person_present(true);
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
	BOOST_TEST(access->frame(nullptr, gui::theme_semantic_colors{}));
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
}

// Headless: no person even when questions would be routed; a waiting question is declined, a new ask is a deny.
BOOST_AUTO_TEST_CASE(HeadlessFrame_NobodyIsAsked_EvenWithQuestionsRouted)
{
	person_arrives();
	ask_as("s1");
	BOOST_REQUIRE(access->confirmations().pending().size() == 1u);

	BOOST_TEST(access->frame_headless());

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(access->confirmations().pending().empty());

	ask_as("s1");
	BOOST_REQUIRE(finished.size() == 2u);
	BOOST_TEST(finished[1].code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

// Headless (#452 PR-3): whatever frame(), the font check or the routing would say, nobody is asked.
BOOST_AUTO_TEST_CASE(Headless_NobodyIsAsked_EvenWithAReadableFontAndRoutedQuestions)
{
	access->set_headless(true);
	// What frame() does with a font that has Cyrillic, and what the bridge does.
	person_arrives();
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(access->confirmations().pending().empty());
	BOOST_TEST(access->policy().headless());
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(TurningHeadless_DeclinesWhatWaits)
{
	person_arrives();
	ask_as("s1");
	BOOST_REQUIRE(access->confirmations().pending().size() == 1u);

	access->set_headless(true);

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(access->confirmations().pending().empty());
}

BOOST_AUTO_TEST_CASE(Headless_TheAgentPolicyFile_AnswersAnAskInAdvance)
{
	dir.write("answers.json", R"({"categories":{"code_files":"allow"}})");
	access->policy().set_preanswers_file(dir.path() / "answers.json", project_root());
	access->set_headless(true);
	person_arrives();
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].ok);
	BOOST_TEST(runs == 1);
}

// With a window the file never answers, also when nobody can be asked there (a font without Cyrillic, before the first frame).
BOOST_AUTO_TEST_CASE(WithAWindowAndNobodyToAsk_TheAgentPolicyFile_DoesNotAnswer)
{
	dir.write("answers.json", R"({"categories":{"code_files":"allow"}})");
	access->policy().set_preanswers_file(dir.path() / "answers.json", project_root());
	access->set_questions_routed(true);
	access->set_person_present(false);
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(WithAPerson_TheAgentPolicyFile_DoesNotAnswer)
{
	dir.write("answers.json", R"({"categories":{"code_files":"allow"}})");
	access->policy().set_preanswers_file(dir.path() / "answers.json", project_root());
	person_arrives();
	ask_as("s1");

	BOOST_TEST(finished.empty());
	BOOST_TEST(access->confirmations().pending().size() == 1u);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(QuestionsNotRoutedYet_AnAskIsADenyEvenWithAPerson)
{
	access->set_person_present(true);
	ask_as("s1");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(access->confirmations().pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(TheFontStopsBeingReadable_WaitingRequestsAreDeclinedAndNewOnesDenied)
{
	person_arrives();
	ask_as("s1");
	BOOST_REQUIRE(access->confirmations().pending().size() == 1u);

	BOOST_TEST(access->frame(nullptr, gui::theme_semantic_colors{}));

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(access->confirmations().pending().empty());
	ask_as("s1");
	BOOST_REQUIRE(finished.size() == 2u);
	BOOST_TEST(finished[1].code == cmd::codes::denied);
}

BOOST_AUTO_TEST_CASE(EveryDefaultReadTool_NeedsNoQuestion)
{
	cmd::command_desc desc;
	desc.id = "log.read";
	desc.category = agent_category::read;
	desc.args_schema = boost::json::value{ { "type", "object" } };
	desc.invoke = [](const cmd::call&) { return cmd::result::success(); };
	registrations.push_back(commands.add(std::move(desc)));

	cmd::call request;
	request.args = boost::json::object{};
	request.from = cmd::origin::agent("s1");
	BOOST_TEST(commands.invoke("log.read", request).ok);
}

BOOST_AUTO_TEST_CASE(PersonPresent_AnAskWaitsForTheAnswer)
{
	person_arrives();
	ask_as("s1");

	BOOST_TEST(finished.empty());
	BOOST_REQUIRE(access->confirmations().pending().size() == 1u);
}

BOOST_AUTO_TEST_CASE(SessionClosed_DeclinesOnlyThatSessionsRequests)
{
	person_arrives();
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
	person_arrives();
	ask_as("s1");
	test_util::show_confirmations_and_wait(access->confirmations(), current);
	access->confirmations().answer(access->confirmations().pending().front().id, edt::confirmation_answer::allow_session);
	BOOST_TEST(access->grants().has("s1", agent_category::code_files));

	bridge->session_closed("s1");
	tasks.drain_main(std::chrono::hours(1));

	BOOST_TEST(!access->grants().has("s1", agent_category::code_files));
}

BOOST_AUTO_TEST_CASE(DestroyingTheAccess_AnswersWaitingRequestsAndRemovesTheGate)
{
	person_arrives();
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

// #935: the filter the editor installs for log.read, over the real policy and the registry's gate.
BOOST_AUTO_TEST_CASE(AnAgentsLogRead_HasTheBuildOnlyWhileThePolicyAllowsTheBuild_APersonReadsEverythingAsItIs)
{
	struct journal_scope
	{
		std::size_t capacity = engine::log_journal_capacity();
		std::optional<engine::log_level> level = engine::log_journal_min_level();
		journal_scope()
		{
			engine::set_log_journal_capacity(1000);
			engine::set_log_journal_min_level(engine::log_level::trace);
		}
		~journal_scope()
		{
			engine::set_log_journal_capacity(capacity);
			engine::set_log_journal_min_level(level);
		}
	} journal;

	cmd::command_desc build;
	build.id = "build.game_module";
	build.category = agent_category::build;
	build.risk = cmd::danger::deny_by_default;
	build.summary = "builds the game module";
	build.args_schema = boost::json::value{ { "type", "object" } };
	build.invoke = [](const cmd::call&) { return cmd::result::success(); };
	registrations.push_back(commands.add(std::move(build)));

	cmd::log_read_filter filter;
	edt::agent_log_roots roots;
	roots.project = { dir.path() };
	// Again after every change of the policy: the filter keeps the policy's answer for a moment.
	const auto install = [&] {
		edt::install_agent_log_filter(filter, [this] { return edt::agent_build_output_allowed(access->policy(), commands); }, roots);
	};
	registrations.push_back(cmd::register_log_commands(commands, &filter));

	const std::uint64_t first = engine::log_journal_next_seq();
	const std::string file = (dir.path() / "code" / "a.cpp").generic_string();
	egLOG_WARN("editor/build", "{}: error: boom", file);
	egLOG_INFO("edt_agent_access_log_test", "saved {}", file);
	const auto read = [&](const char* category, cmd::origin from) {
		const cmd::result got = commands.invoke("log.read",
			boost::json::object{ { "category", category }, { "since_seq", first } }, std::move(from));
		BOOST_REQUIRE_MESSAGE(got.ok, got.error);
		std::vector<std::string> texts;
		for (const auto& entry : got.value.as_object().at("entries").as_array()) {
			texts.emplace_back(entry.as_object().at("text").as_string());
		}
		return texts;
	};
	const std::vector<std::string> raw_build{ file + ": error: boom" };

	dir.write(".snk/agent_policy.json", R"({"categories":{"build":"deny"}})");
	install();
	BOOST_TEST(!edt::agent_build_output_allowed(access->policy(), commands));
	BOOST_TEST(read("editor/build", cmd::origin::agent("s1")).empty());
	BOOST_TEST((read("edt_agent_access_log_test", cmd::origin::agent("s1")) == std::vector<std::string>{ "saved <project>/code/a.cpp" }));
	BOOST_TEST((read("editor/build", cmd::origin::ui()) == raw_build));
	BOOST_TEST((read("edt_agent_access_log_test", cmd::origin::ui()) == std::vector<std::string>{ "saved " + file }));

	// Asking a person before each build is not leave to read what a build printed.
	const auto approve_as_a_person = [this](const char* policy_text) {
		dir.write(".snk/agent_policy.json", policy_text);
		access->policy().set_approval_allowed(true);
		access->policy().refresh();
		const edt::policy_layer reviewed = access->policy().project();
		const std::optional<std::string> refused = access->policy().approve_project_layer(reviewed);
		BOOST_REQUIRE_MESSAGE(!refused.has_value(), refused.value_or(""));
	};
	approve_as_a_person(R"({"commands":{"build.game_module":"ask"}})");
	install();
	BOOST_TEST(read("editor/build", cmd::origin::agent("s1")).empty());

	approve_as_a_person(R"({"commands":{"build.game_module":"allow"}})");
	install();
	BOOST_TEST(edt::agent_build_output_allowed(access->policy(), commands));
	BOOST_TEST((read("editor/build", cmd::origin::agent("s1")) == std::vector<std::string>{ "<project>/code/a.cpp: error: boom" }));
	BOOST_TEST((read("editor/build", cmd::origin::ui()) == raw_build));
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
