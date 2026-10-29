#include <boost/test/unit_test.hpp>
#include "edt_agent_confirmations.h"
#include "edt_agent_policy.h"
#include "test_temp_dir.hpp"
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The question to a person behind every `ask` (A6b #293): 60 s, three answers, session grants, and the call that runs
// once a person said yes.
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using cmd::agent_category;

struct confirmations_fixture
{
	test_util::temp_dir dir{ "edt_agent_confirmations" };
	cmd::command_registry commands;
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	edt::session_grants grants;
	std::shared_ptr<edt::agent_policy_gate> gate = std::make_shared<edt::agent_policy_gate>(policy);
	// The question's clock, and the registry's own (its tickets live longer than a question).
	std::chrono::steady_clock::time_point current{};
	std::chrono::steady_clock::time_point registry_time{};
	std::unique_ptr<edt::agent_confirmations> confirmations;
	std::vector<cmd::registration> registrations;
	int runs = 0;
	std::vector<cmd::result> finished;

	confirmations_fixture()
	{
		add("scene.move", agent_category::scene_edit);
		add("code.create", agent_category::code_files);
		add("cmake.edit", agent_category::cmake_presets);
		add("build.run", agent_category::build);
		gate->set_can_ask(true);
		gate->set_session_grants(&grants);
		commands.set_policy_gate(gate);
		commands.set_clock([this] { return registry_time; });
		confirmations = std::make_unique<edt::agent_confirmations>(commands, grants, [this] { return current; });
	}

	// The destructor of agent_confirmations answers what still waits, into `finished`: it must go before the members it uses.
	~confirmations_fixture()
	{
		confirmations.reset();
	}

	void add(std::string id, agent_category category)
	{
		cmd::command_desc desc;
		desc.id = std::move(id);
		desc.category = category;
		desc.summary = "does " + desc.id;
		desc.invoke = [this](const cmd::call&) {
			++runs;
			return cmd::result::success();
		};
		registrations.push_back(commands.add(std::move(desc)));
	}

	cmd::call agent_call(std::string session = "s1") const
	{
		cmd::call request;
		request.args = boost::json::object{ { "path", "src/a.cpp" } };
		request.from = cmd::origin::agent(std::move(session));
		return request;
	}

	void run(const std::string& id, std::string session = "s1")
	{
		cmd::call request = agent_call(std::move(session));
		request.args = boost::json::object{};
		confirmations->invoke(id, std::move(request), [this](cmd::result outcome) { finished.push_back(std::move(outcome)); });
	}

	std::uint64_t only_pending_id() const
	{
		const auto pending = confirmations->pending();
		BOOST_REQUIRE(pending.size() == 1u);
		return pending.front().id;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentConfirmations, confirmations_fixture)

BOOST_AUTO_TEST_CASE(Allow_RunsAtOnce_WithoutAQuestion)
{
	run("scene.move");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].ok);
	BOOST_TEST(confirmations->pending().empty());
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_CASE(Deny_AnswersAtOnce_WithoutAQuestion)
{
	run("build.run");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(confirmations->pending().empty());
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(Ask_WaitsForAPerson_AndDescribesTheCall)
{
	cmd::call request = agent_call("agent-7");
	confirmations->invoke("code.create", request, [this](cmd::result outcome) { finished.push_back(std::move(outcome)); });

	BOOST_TEST(finished.empty());
	BOOST_TEST(runs == 0);
	const auto pending = confirmations->pending();
	BOOST_REQUIRE(pending.size() == 1u);
	BOOST_TEST(pending[0].command == "code.create");
	BOOST_TEST(pending[0].session == "agent-7");
	BOOST_TEST((pending[0].category == agent_category::code_files));
	BOOST_TEST(pending[0].args.find("src/a.cpp") != std::string::npos);
	BOOST_TEST(pending[0].summary == "does code.create");
	BOOST_TEST((pending[0].deadline - pending[0].asked_at == edt::agent_confirmations::timeout));
}

BOOST_AUTO_TEST_CASE(TheSameRequestAskedAgain_IsNotAskedTwice)
{
	run("code.create");
	run("code.create");

	BOOST_TEST(confirmations->pending().size() == 1u);
	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(finished[0].error.find("already waiting") != std::string::npos);

	confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_once);
	BOOST_TEST(finished.size() == 2u);
	BOOST_TEST(finished[1].ok);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_CASE(AllowOnce_RunsTheCallOnce_AndTheNextCallAsksAgain)
{
	run("code.create");
	BOOST_TEST(confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_once));

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].ok);
	BOOST_TEST(runs == 1);
	BOOST_TEST(confirmations->pending().empty());

	run("code.create");
	BOOST_TEST(finished.size() == 1u);
	BOOST_TEST(confirmations->pending().size() == 1u);
}

BOOST_AUTO_TEST_CASE(Deny_DeclinesTheCall_AndForgetsTheTicket)
{
	run("code.create");
	BOOST_TEST(confirmations->answer(only_pending_id(), edt::confirmation_answer::deny));

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(finished[0].error.find("declined") != std::string::npos);
	BOOST_TEST(runs == 0);
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_CoversThatCategoryOfThatSessionOnly)
{
	run("code.create", "s1");
	confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_session);
	BOOST_TEST(runs == 1);

	run("code.create", "s1");
	BOOST_TEST(finished.size() == 2u);
	BOOST_TEST(finished[1].ok);
	BOOST_TEST(runs == 2);
	BOOST_TEST(confirmations->pending().empty());

	run("code.create", "s2");
	BOOST_TEST(confirmations->pending().size() == 1u);
	confirmations->answer(only_pending_id(), edt::confirmation_answer::deny);

	run("cmake.edit", "s1");
	BOOST_TEST(confirmations->pending().size() == 1u);
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_IsDroppedWithTheSession)
{
	run("code.create", "s1");
	confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_session);
	BOOST_TEST(grants.has("s1", agent_category::code_files));

	confirmations->drop_session("s1");

	BOOST_TEST(!grants.has("s1", agent_category::code_files));
	run("code.create", "s1");
	BOOST_TEST(confirmations->pending().size() == 1u);
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_NeverLiftsAProjectDeny)
{
	run("code.create", "s1");
	confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_session);
	BOOST_TEST(runs == 1);

	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"deny"}})");
	run("code.create", "s1");

	BOOST_TEST(finished.back().code == cmd::codes::denied);
	BOOST_TEST(runs == 1);
	BOOST_TEST(confirmations->pending().empty());
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_IsKeptInMemoryOnly_NothingIsWrittenAnywhere)
{
	const auto files_under_dir = [this] {
		std::map<std::string, std::string> files;
		for (const auto& entry : fs::recursive_directory_iterator(dir.path())) {
			if (!entry.is_regular_file()) continue;
			std::ifstream in(entry.path(), std::ios::binary);
			files[entry.path().generic_string()] = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
		return files;
	};
	const auto before = files_under_dir();

	run("code.create", "s1");
	confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_session);
	BOOST_TEST(grants.has("s1", agent_category::code_files));

	BOOST_TEST((files_under_dir() == before));
	// A fresh editor over the same files knows nothing of it.
	edt::agent_policy again{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((again.effective(*commands.find("code.create")).mode == cmd::policy_mode::ask));
	edt::session_grants fresh;
	BOOST_TEST(!fresh.has("s1", agent_category::code_files));
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_NeverLiftsAPersonalDenyEither)
{
	run("code.create", "s1");
	confirmations->answer(only_pending_id(), edt::confirmation_answer::allow_session);
	dir.write("personal.json", R"({"categories":{"code_files":"deny"}})");

	run("code.create", "s1");

	BOOST_TEST(finished.back().code == cmd::codes::denied);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_CASE(WithNobodyToAsk_TheAnswerIsAnImmediateDeny)
{
	gate->set_can_ask(false);

	run("code.create");

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(confirmations->pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(AnAnswerPastTheDeadline_IsRefused_EvenBeforeTick)
{
	run("code.create");
	const std::uint64_t id = only_pending_id();

	current += 61s;

	BOOST_TEST(confirmations->pending().empty());
	BOOST_TEST(!confirmations->answer(id, edt::confirmation_answer::allow_once));
	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(finished[0].error.find("nobody answered") != std::string::npos);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(ACommandUnregisteredWhileItsRequestWaits_IsDenied)
{
	run("code.create");
	const std::uint64_t id = only_pending_id();

	registrations[1].reset();
	BOOST_TEST(commands.find("code.create") == nullptr);

	BOOST_TEST(confirmations->answer(id, edt::confirmation_answer::allow_once));
	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(LongArguments_ReachTheDialogWhole)
{
	boost::json::object args;
	args["note"] = std::string(5000, 'x');
	args["path"] = "../CMakeLists.txt";
	cmd::call request = agent_call("s1");
	request.args = args;
	confirmations->invoke("code.create", std::move(request), [this](cmd::result outcome) { finished.push_back(std::move(outcome)); });

	const auto pending = confirmations->pending();

	BOOST_REQUIRE(pending.size() == 1u);
	BOOST_TEST(pending[0].args.size() > 5000u);
	BOOST_TEST(pending[0].args.find("../CMakeLists.txt") != std::string::npos);
	BOOST_TEST(pending[0].args.find("\"note\"") != std::string::npos);
	BOOST_TEST(pending[0].args.find("...") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_SettlesTheWaitingRequestsOfThatSessionAndCategory)
{
	cmd::call first = agent_call("s1");
	first.args = boost::json::object{ { "n", 1 } };
	cmd::call second = agent_call("s1");
	second.args = boost::json::object{ { "n", 2 } };
	const auto record = [this](cmd::result outcome) { finished.push_back(std::move(outcome)); };
	confirmations->invoke("code.create", first, record);
	confirmations->invoke("code.create", second, record);
	run("cmake.edit", "s1");
	run("code.create", "s2");
	BOOST_REQUIRE(confirmations->pending().size() == 4u);
	const std::uint64_t id = confirmations->pending().front().id;

	confirmations->answer(id, edt::confirmation_answer::allow_session);

	BOOST_TEST(runs == 2);
	BOOST_REQUIRE(finished.size() == 2u);
	BOOST_TEST(finished[0].ok);
	BOOST_TEST(finished[1].ok);
	const auto left = confirmations->pending();
	BOOST_REQUIRE(left.size() == 2u);
	BOOST_TEST(left[0].command == "cmake.edit");
	BOOST_TEST(left[1].session == "s2");
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_DoesNotApproveARequestThatAlreadyTimedOut)
{
	cmd::call early = agent_call("s1");
	early.args = boost::json::object{ { "n", 1 } };
	cmd::call later = agent_call("s1");
	later.args = boost::json::object{ { "n", 2 } };
	const auto record = [this](cmd::result outcome) { finished.push_back(std::move(outcome)); };
	confirmations->invoke("code.create", early, record);
	current += 50s;
	confirmations->invoke("code.create", later, record);
	current += 20s;
	// The first is past its deadline (no tick() yet) and hidden; the second still waits.
	const auto visible = confirmations->pending();
	BOOST_REQUIRE(visible.size() == 1u);

	BOOST_TEST(confirmations->answer(visible.front().id, edt::confirmation_answer::allow_session));

	BOOST_TEST(runs == 1);
	BOOST_REQUIRE(finished.size() == 2u);
	BOOST_TEST(finished[0].ok);
	BOOST_TEST(finished[1].code == cmd::codes::denied);
	BOOST_TEST(finished[1].error.find("nobody answered") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(AllowForTheSession_AfterThePolicyTurnedToDeny_DeniesTheSettledRequestsToo)
{
	cmd::call first = agent_call("s1");
	first.args = boost::json::object{ { "n", 1 } };
	cmd::call second = agent_call("s1");
	second.args = boost::json::object{ { "n", 2 } };
	const auto record = [this](cmd::result outcome) { finished.push_back(std::move(outcome)); };
	confirmations->invoke("code.create", first, record);
	confirmations->invoke("code.create", second, record);
	BOOST_REQUIRE(confirmations->pending().size() == 2u);
	const std::uint64_t id = confirmations->pending().front().id;

	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"deny"}})");
	confirmations->answer(id, edt::confirmation_answer::allow_session);

	BOOST_TEST(runs == 0);
	BOOST_REQUIRE(finished.size() == 2u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(finished[1].code == cmd::codes::denied);
	BOOST_TEST(confirmations->pending().empty());
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_AUTO_TEST_CASE(TheGrant_IsGivenOnlyWhenTheTicketIsApproved)
{
	run("code.create", "s1");
	const std::uint64_t id = only_pending_id();
	// The registry's ticket is gone (it lives 120 s) while the question (60 s) still waits.
	registry_time += cmd::command_registry::confirmation_lifetime + 1s;

	BOOST_TEST(confirmations->answer(id, edt::confirmation_answer::allow_session));

	BOOST_REQUIRE(finished.size() == 1u);
	BOOST_TEST(finished[0].code == cmd::codes::denied);
	BOOST_TEST(finished[0].error.find("expired") != std::string::npos);
	BOOST_TEST(!grants.has("s1", agent_category::code_files));
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_SUITE_END()
