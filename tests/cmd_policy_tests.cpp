#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_policy.h>
#include <snk/command/cmd_registry.h>
#include <boost/json/object.hpp>
#include <algorithm>
#include <cstdint>
#include <chrono>
#include <memory>
#include <string>
#include <utility>

// The policy gate inside command_registry::invoke (A6b #293): who is asked, what each answer does, and the one-time
// tickets of a confirmation.
namespace {

using namespace std::chrono_literals;

struct fake_gate final : cmd::policy_gate
{
	cmd::policy_mode mode = cmd::policy_mode::allow;
	std::string reason;
	bool person_present = true;
	int asked = 0;

	cmd::policy_decision decide(const cmd::command_desc&, const cmd::call&) override
	{
		++asked;
		return { mode, reason };
	}
	bool can_ask() const override { return person_present; }
};

struct policy_fixture
{
	cmd::command_registry commands;
	std::shared_ptr<fake_gate> gate = std::make_shared<fake_gate>();
	std::chrono::steady_clock::time_point current{};
	int runs = 0;
	cmd::registration registration;

	policy_fixture()
	{
		commands.set_clock([this] { return current; });
		cmd::command_desc desc;
		desc.id = "test.act";
		desc.args_schema = boost::json::value{ { "type", "object" },
			{ "properties", boost::json::object{ { "n", boost::json::object{ { "type", "integer" } } } } }, { "additionalProperties", false } };
		desc.invoke = [this](const cmd::call&) {
			++runs;
			return cmd::result::success();
		};
		registration = commands.add(std::move(desc));
		commands.set_policy_gate(gate);
	}

	cmd::call from_agent(std::int64_t n = 1, std::string session = "s1", std::string ticket = {}) const
	{
		cmd::call request;
		request.args = boost::json::object{ { "n", n } };
		request.from = cmd::origin::agent(std::move(session));
		request.confirmation = std::move(ticket);
		return request;
	}

	std::string ticket_of(const cmd::result& answer) const
	{
		return std::string{ answer.value.as_object().at("ticket").as_string().c_str() };
	}
};

}

BOOST_AUTO_TEST_SUITE(CmdPolicyTests)

BOOST_AUTO_TEST_CASE(Strictest_IsTheLargerOfTheTwoModes_ForEveryPair)
{
	using cmd::policy_mode;
	const policy_mode all[] = { policy_mode::allow, policy_mode::ask, policy_mode::deny };
	for (int a = 0; a < 3; ++a) {
		for (int b = 0; b < 3; ++b) {
			BOOST_TEST_INFO(a << "," << b);
			BOOST_TEST((cmd::strictest(all[a], all[b]) == all[std::max(a, b)]));
			BOOST_TEST((cmd::strictest(all[a], all[b]) == cmd::strictest(all[b], all[a])));
		}
	}
	BOOST_TEST(std::string{ cmd::to_string(policy_mode::ask) } == "ask");
}

BOOST_FIXTURE_TEST_CASE(NoGate_NothingIsAsked, policy_fixture)
{
	commands.set_policy_gate(nullptr);

	BOOST_TEST(commands.invoke("test.act", from_agent()).ok);
	BOOST_TEST(runs == 1);
}

BOOST_FIXTURE_TEST_CASE(Allow_RunsTheCommand, policy_fixture)
{
	gate->mode = cmd::policy_mode::allow;

	BOOST_TEST(commands.invoke("test.act", from_agent()).ok);
	BOOST_TEST(runs == 1);
	BOOST_TEST(gate->asked == 1);
}

BOOST_FIXTURE_TEST_CASE(Deny_RefusesAndTheCommandDoesNotRun, policy_fixture)
{
	gate->mode = cmd::policy_mode::deny;
	gate->reason = "builds are off";

	const auto answer = commands.invoke("test.act", from_agent());

	BOOST_TEST(!answer.ok);
	BOOST_TEST(answer.code == cmd::codes::denied);
	BOOST_TEST(answer.error.find("builds are off") != std::string::npos);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(OnlyAgentsAreAsked_UiTestAndGameRunWhateverTheGateSays, policy_fixture)
{
	gate->mode = cmd::policy_mode::deny;

	for (const cmd::origin& from : { cmd::origin::ui(), cmd::origin::test(), cmd::origin::game() }) {
		cmd::call request;
		request.args = boost::json::object{};
		request.from = from;
		BOOST_TEST(commands.invoke("test.act", request).ok);
	}
	BOOST_TEST(runs == 3);
	BOOST_TEST(gate->asked == 0);
}

BOOST_FIXTURE_TEST_CASE(Deny_IsDecidedBeforeTheArgumentsAreChecked, policy_fixture)
{
	gate->mode = cmd::policy_mode::deny;
	cmd::call request;
	request.args = boost::json::object{ { "n", "not an integer" } };
	request.from = cmd::origin::agent("s1");

	BOOST_TEST(commands.invoke("test.act", request).code == cmd::codes::denied);
}

BOOST_FIXTURE_TEST_CASE(Ask_WithNobodyToAsk_IsDenied, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	gate->person_present = false;

	const auto answer = commands.invoke("test.act", from_agent());

	BOOST_TEST(answer.code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Ask_GivesATicketAndDoesNotRun, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	gate->reason = "creates a file";

	const auto answer = commands.invoke("test.act", from_agent());

	BOOST_TEST(answer.code == cmd::codes::confirmation_required);
	BOOST_TEST(!ticket_of(answer).empty());
	BOOST_TEST(answer.value.as_object().at("reason").as_string() == "creates a file");
	BOOST_TEST(runs == 0);
	BOOST_TEST(commands.pending_confirmations() == 1u);
}

BOOST_FIXTURE_TEST_CASE(ApprovedTicket_RunsTheSameCallOnce, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	const std::string ticket = ticket_of(commands.invoke("test.act", from_agent(7)));
	BOOST_TEST(commands.approve(ticket));

	BOOST_TEST(commands.invoke("test.act", from_agent(7, "s1", ticket)).ok);
	BOOST_TEST(runs == 1);

	const auto again = commands.invoke("test.act", from_agent(7, "s1", ticket));
	BOOST_TEST(again.code == cmd::codes::denied);
	BOOST_TEST(runs == 1);
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Ticket_DoesNotWorkForOtherArguments_AnotherSession_OrBeforeTheApproval, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	const std::string ticket = ticket_of(commands.invoke("test.act", from_agent(7)));

	BOOST_TEST(commands.invoke("test.act", from_agent(7, "s1", ticket)).code == cmd::codes::denied);
	BOOST_TEST(commands.approve(ticket));
	BOOST_TEST(commands.invoke("test.act", from_agent(8, "s1", ticket)).code == cmd::codes::denied);
	BOOST_TEST(commands.invoke("test.act", from_agent(7, "other", ticket)).code == cmd::codes::denied);
	BOOST_TEST(runs == 0);

	BOOST_TEST(commands.invoke("test.act", from_agent(7, "s1", ticket)).ok);
	BOOST_TEST(runs == 1);
}

BOOST_FIXTURE_TEST_CASE(Ticket_OfAnotherCommand_DoesNotWork, policy_fixture)
{
	cmd::command_desc other;
	other.id = "test.other";
	other.invoke = [this](const cmd::call&) {
		++runs;
		return cmd::result::success();
	};
	auto other_registration = commands.add(std::move(other));
	gate->mode = cmd::policy_mode::ask;
	const std::string ticket = ticket_of(commands.invoke("test.act", from_agent(1)));
	commands.approve(ticket);

	cmd::call request;
	request.args = boost::json::object{ { "n", 1 } };
	request.from = cmd::origin::agent("s1");
	request.confirmation = ticket;
	BOOST_TEST(commands.invoke("test.other", request).code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(Ticket_Expires, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	const std::string ticket = ticket_of(commands.invoke("test.act", from_agent()));
	commands.approve(ticket);

	current += cmd::command_registry::confirmation_lifetime + 1s;

	BOOST_TEST(commands.invoke("test.act", from_agent(1, "s1", ticket)).code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
	BOOST_TEST(!commands.approve(ticket));
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Discard_ForgetsTheTicket, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	const std::string ticket = ticket_of(commands.invoke("test.act", from_agent()));

	commands.discard(ticket);

	BOOST_TEST(!commands.approve(ticket));
	BOOST_TEST(commands.pending_confirmations() == 0u);
}

BOOST_FIXTURE_TEST_CASE(ApprovedTicket_IsNotEnoughWhenThePolicyTurnedToDeny, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	const std::string ticket = ticket_of(commands.invoke("test.act", from_agent()));
	commands.approve(ticket);

	gate->mode = cmd::policy_mode::deny;

	BOOST_TEST(commands.invoke("test.act", from_agent(1, "s1", ticket)).code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(NoGuessedTicket_Works, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;

	BOOST_TEST(commands.invoke("test.act", from_agent(1, "s1", "0000000000000000")).code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(TooManyPendingTickets_AreRefused, policy_fixture)
{
	gate->mode = cmd::policy_mode::ask;
	for (std::size_t i = 0; i < cmd::command_registry::max_pending_confirmations; ++i) {
		BOOST_REQUIRE(commands.invoke("test.act", from_agent(static_cast<std::int64_t>(i))).code == cmd::codes::confirmation_required);
	}

	BOOST_TEST(commands.invoke("test.act", from_agent(1000)).code == cmd::codes::denied);
}

BOOST_FIXTURE_TEST_CASE(DisabledCommand_StaysDisabled_NotAskedAbout, policy_fixture)
{
	cmd::command_desc off;
	off.id = "test.off";
	off.invoke = [](const cmd::call&) { return cmd::result::success(); };
	off.enabled = [] { return false; };
	auto off_registration = commands.add(std::move(off));
	gate->mode = cmd::policy_mode::deny;
	const int before = gate->asked;

	cmd::call request;
	request.args = boost::json::object{};
	request.from = cmd::origin::agent("s1");
	BOOST_TEST(commands.invoke("test.off", request).code == cmd::codes::disabled);
	BOOST_TEST(gate->asked == before);
}

BOOST_AUTO_TEST_SUITE_END()
