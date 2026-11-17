#include <boost/test/unit_test.hpp>
#include "edt_forge_pr_dialog_model.h"
#include "edt_forge_service.h"
#include <snk/command/cmd_types.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// The Create PR form's model (#458 PR-4): what it sends to forge.create_pr and what it makes of the job's answer.
namespace {

namespace json = boost::json;

struct dialog_fixture
{
	struct call
	{
		std::string id;
		json::value args;
		std::uint64_t job = 0;
	};

	edt::editor_events events;
	std::vector<call> calls;
	std::vector<std::uint64_t> cancelled;
	std::uint64_t next_job = 1;
	std::optional<cmd::result> next_answer;
	int described = 0;
	edt::forge_service::target_description next_target = shown_target();
	edt::forge_pr_dialog_model model{ events, ports() };

	edt::forge_pr_dialog_model::ports ports()
	{
		edt::forge_pr_dialog_model::ports out;
		out.invoke = [this](std::string_view id, json::value args) {
			const std::uint64_t job = next_job++;
			calls.push_back({ std::string(id), std::move(args), job });
			if (next_answer) return *next_answer;
			return cmd::result::success(json::object{ { "job", job } });
		};
		out.cancel = [this](std::uint64_t job) {
			cancelled.push_back(job);
			return true;
		};
		out.describe_target = [this](const std::string&) {
			++described;
			return next_target;
		};
		return out;
	}

	static edt::forge_service::target_description shown_target()
	{
		edt::forge_service::target_description target;
		target.ok = true;
		target.text = "github.com/octo/engine (remote origin)";
		return target;
	}

	static edt::forge_service::target_description rewritten_target()
	{
		edt::forge_service::target_description target = shown_target();
		target.rewritten = true;
		target.written_url = "https://github.com/octo/engine.git";
		target.effective_url = "https://github.com/someone/else.git";
		return target;
	}

	void fill(const std::string& title = "Add the forge layer", const std::string& body = "Why and how.")
	{
		model.reset("agent/forge");
		model.title() = title;
		model.body() = body;
	}

	void finish(json::value result, edt::job_status status = edt::job_status::done)
	{
		BOOST_REQUIRE(!calls.empty());
		events.job_finished.emit(calls.back().job, status, std::string{}, std::optional<json::value>{ std::move(result) });
	}

	static json::value created()
	{
		return json::parse(R"({"number":42,"title":"Add the forge layer","state":"open","draft":true,"head":"agent/forge","base":"main",
			"url":"https://github.com/octo/engine/pull/42"})");
	}

	static json::value failure(std::string_view code, std::string_view message)
	{
		return json::object{ { "error", json::object{ { "code", json::string(code) }, { "message", json::string(message) } } } };
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtForgePrDialogModel, dialog_fixture)

BOOST_AUTO_TEST_CASE(AFreshForm_IsADraftIntoMain_FromTheCurrentBranch)
{
	model.reset("feature/x");
	BOOST_TEST(model.base() == "main");
	BOOST_TEST(model.head() == "feature/x");
	BOOST_TEST(model.title().empty());
	BOOST_TEST(model.body().empty());
	BOOST_TEST(model.draft());
	BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::editing));
	BOOST_TEST(!model.can_create());
}

BOOST_AUTO_TEST_CASE(Create_SendsExactlyBaseHeadTitleBodyAndDraft_AsAPersonsCall)
{
	fill();
	BOOST_REQUIRE(model.can_create());
	model.create();
	BOOST_REQUIRE(calls.size() == 1u);
	BOOST_TEST(calls[0].id == "forge.create_pr");
	BOOST_REQUIRE(calls[0].args.is_object());
	const json::object& args = calls[0].args.as_object();
	std::set<std::string> keys;
	for (const auto& entry : args) keys.insert(std::string(entry.key()));
	const std::set<std::string> expected{ "base", "head", "title", "body", "draft" };
	BOOST_TEST((keys == expected));
	BOOST_TEST(std::string(args.at("base").as_string()) == "main");
	BOOST_TEST(std::string(args.at("head").as_string()) == "agent/forge");
	BOOST_TEST(std::string(args.at("title").as_string()) == "Add the forge layer");
	BOOST_TEST(std::string(args.at("body").as_string()) == "Why and how.");
	BOOST_TEST(args.at("draft").as_bool());
	BOOST_TEST(model.running());
}

BOOST_AUTO_TEST_CASE(ThePerson_MayTurnTheDraftOff)
{
	fill();
	model.draft() = false;
	model.create();
	BOOST_REQUIRE(calls.size() == 1u);
	BOOST_TEST(!calls[0].args.as_object().at("draft").as_bool());
}

BOOST_AUTO_TEST_CASE(AnEmptyBody_IsStillSent)
{
	fill("Title", "");
	model.create();
	BOOST_REQUIRE(calls.size() == 1u);
	BOOST_TEST(calls[0].args.as_object().at("body").as_string().empty());
}

BOOST_AUTO_TEST_CASE(TheBody_MayHaveTabsAndLineBreaks)
{
	fill("Title", "line one\n\tline two\r\nline three");
	BOOST_TEST(!model.input_error().has_value());
	model.create();
	BOOST_TEST(calls.size() == 1u);
}

BOOST_AUTO_TEST_CASE(BadInput_IsRefusedHere_WithNoCall)
{
	const std::vector<std::pair<std::string, std::string>> bad_titles{
		{ "", "empty" },
		{ std::string(edt::forge_service::k_max_title + 1, 'a'), "too long" },
		{ "tab\there", "tab" },
		{ "two\nlines", "line break" },
		{ std::string("bell") + '\x07', "control" },
		{ "bidi \xE2\x80\xAE text", "bidi" },
		{ "bad utf8 \xC3", "invalid UTF-8" },
	};
	for (const auto& [title, why] : bad_titles) {
		BOOST_TEST_CONTEXT(why) {
			fill(title);
			BOOST_TEST(model.input_error().has_value());
			BOOST_TEST(!model.can_create());
			model.create();
			BOOST_TEST(model.error_code() == "invalid_args");
			BOOST_TEST(!model.error_text().empty());
			BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::editing));
		}
	}
	fill("Title", std::string(edt::forge_service::k_max_body + 1, 'b'));
	model.create();
	BOOST_TEST(model.error_code() == "invalid_args");
	fill("Title", std::string("bell") + '\x07');
	model.create();
	BOOST_TEST(model.error_code() == "invalid_args");
	fill();
	model.head() = "bad..name";
	model.create();
	BOOST_TEST(model.error_code() == "invalid_args");
	fill();
	model.base() = "-rf";
	model.create();
	BOOST_TEST(model.error_code() == "invalid_args");
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(ALimitSizedInput_IsAccepted)
{
	fill(std::string(edt::forge_service::k_max_title, 'a'), std::string(edt::forge_service::k_max_body, 'b'));
	BOOST_TEST(!model.input_error().has_value());
}

BOOST_AUTO_TEST_CASE(Success_ShowsTheNumberAndTheAddress)
{
	fill();
	model.create();
	finish(created());
	BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::created));
	BOOST_TEST(model.number().value_or(0) == 42);
	BOOST_TEST(model.url() == "https://github.com/octo/engine/pull/42");
	BOOST_TEST(model.error_code().empty());
}

BOOST_AUTO_TEST_CASE(TheAddress_IsKeptOnlyAsAnHttpsLink_AndPlain)
{
	fill();
	model.create();
	finish(json::parse(R"json({"number":7,"url":"javascript:alert(1)"})json"));
	BOOST_TEST(model.number().value_or(0) == 7);
	BOOST_TEST(model.url().empty());

	model.reset("agent/forge");
	fill();
	model.create();
	finish(json::object{ { "number", 8 }, { "url", json::string("https://github.com/o/r/pull/8\x01\xE2\x80\xAE") } });
	BOOST_TEST(model.url().find('\x01') == std::string::npos);
	BOOST_TEST(model.url().find("\xE2\x80\xAE") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(NoToken_PointsToTheAccountWindow)
{
	fill();
	model.create();
	finish(failure("no_token", "no token is stored for github.com"));
	BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::editing));
	BOOST_TEST(model.error_code() == "no_token");
	BOOST_TEST(model.needs_token());
	BOOST_TEST(model.error_hint().find("Git Account") != std::string::npos);
	BOOST_TEST(model.can_create());
}

BOOST_AUTO_TEST_CASE(ErrorsByCode_KeepTheForm_AndCarryAHint)
{
	for (const char* code : { "unauthorized", "forbidden", "validation", "rate_limited", "network", "unavailable", "denied", "invalid_args",
		"no_local_branch", "not_a_repository" }) {
		BOOST_TEST_CONTEXT(code) {
			fill();
			model.create();
			finish(failure(code, "the host said no"));
			BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::editing));
			BOOST_TEST(model.error_code() == code);
			BOOST_TEST(model.error_text() == "the host said no");
			BOOST_TEST(!model.needs_token());
			BOOST_TEST(model.title() == "Add the forge layer");
			if (std::string(code) != "denied" && std::string(code) != "invalid_args") BOOST_TEST(!model.error_hint().empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(ErrorText_IsPlain)
{
	fill();
	model.create();
	finish(failure("network", std::string("line\none\x01") + "two"));
	BOOST_TEST(model.error_text().find('\n') == std::string::npos);
	BOOST_TEST(model.error_text().find('\x01') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(ARefusedCall_IsShown_AndStartsNoJob)
{
	fill();
	next_answer = cmd::result::failure("denied", "the policy says no");
	model.create();
	BOOST_TEST(model.error_code() == "denied");
	BOOST_TEST(model.error_text() == "the policy says no");
	BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::editing));

	fill();
	next_answer = cmd::result::success(json::object{});
	model.create();
	BOOST_TEST(model.error_code() == "failed");
	BOOST_TEST(!model.running());
}

BOOST_AUTO_TEST_CASE(AFailedJob_IsShown)
{
	fill();
	model.create();
	events.job_finished.emit(calls.back().job, edt::job_status::failed, "boom", std::nullopt);
	BOOST_TEST(model.error_code() == "failed");
	BOOST_TEST(model.error_text() == "boom");
	BOOST_TEST(!model.running());
}

BOOST_AUTO_TEST_CASE(WhileItRuns_NothingStartsTwice_AndTheFormStays)
{
	fill();
	model.create();
	model.create();
	BOOST_TEST(calls.size() == 1u);
	BOOST_TEST(!model.can_create());
	model.reset("other");
	BOOST_TEST(model.head() == "agent/forge");
	BOOST_TEST(model.title() == "Add the forge layer");
}

BOOST_AUTO_TEST_CASE(Cancel_StopsTheJob_AndSaysNothingWentWrong)
{
	fill();
	model.create();
	model.cancel();
	BOOST_REQUIRE(cancelled.size() == 1u);
	BOOST_TEST(cancelled[0] == calls[0].job);
	events.job_finished.emit(calls[0].job, edt::job_status::cancelled, std::string{}, std::nullopt);
	BOOST_TEST(!model.running());
	BOOST_TEST(model.error_text().empty());
	BOOST_TEST(model.can_create());
}

BOOST_AUTO_TEST_CASE(AnotherJobsAnswer_IsNotOurs)
{
	fill();
	model.create();
	events.job_finished.emit(calls[0].job + 100, edt::job_status::done, std::string{}, std::optional<json::value>{ created() });
	BOOST_TEST(model.running());
	BOOST_TEST(!model.number().has_value());
}

BOOST_AUTO_TEST_CASE(ANewForm_ForgetsThePreviousPullRequest)
{
	fill();
	model.create();
	finish(created());
	model.reset("feature/y");
	BOOST_TEST((model.state() == edt::forge_pr_dialog_model::phase::editing));
	BOOST_TEST(!model.number().has_value());
	BOOST_TEST(model.url().empty());
	BOOST_TEST(model.error_code().empty());
	BOOST_TEST(model.draft());
}

BOOST_AUTO_TEST_CASE(TheDialog_NeverOpensALink_ItOnlyCallsCreatePr)
{
	fill();
	model.create();
	finish(created());
	for (const call& entry : calls) BOOST_TEST(entry.id == "forge.create_pr");
	BOOST_TEST(calls.size() == 1u);
}

BOOST_AUTO_TEST_CASE(OpeningTheForm_ReadsTheTarget_AndShowsIt)
{
	fill();
	BOOST_TEST(described == 1);
	BOOST_REQUIRE(model.target().has_value());
	BOOST_TEST(model.target()->text == "github.com/octo/engine (remote origin)");
	BOOST_TEST(model.target_accepted());
}

BOOST_AUTO_TEST_CASE(CreateStaysOff_UntilTheTargetIsShownAndAllowed)
{
	next_target = edt::forge_service::target_description{};
	next_target.text = "The request will be refused: the host 'evil.example' is not one pull requests are made on";
	fill();
	BOOST_TEST(!model.can_create());
	BOOST_TEST(!model.target()->ok);
	model.create();
	BOOST_TEST(calls.empty());
	BOOST_TEST(model.error_code() == "denied");

	next_target = shown_target();
	model.refresh_target();
	BOOST_TEST(model.can_create());
}

BOOST_AUTO_TEST_CASE(WithoutADescribePort_NothingCanBeCreated)
{
	edt::forge_pr_dialog_model::ports bare = ports();
	bare.describe_target = nullptr;
	edt::forge_pr_dialog_model blind{ events, std::move(bare) };
	blind.reset("agent/forge");
	blind.title() = "t";
	BOOST_TEST(!blind.can_create());
	blind.create();
	BOOST_TEST(calls.empty());
}

BOOST_AUTO_TEST_CASE(ARewrittenAddress_NeedsTheTick_BeforeCreate)
{
	next_target = rewritten_target();
	fill();
	BOOST_REQUIRE(model.target().has_value());
	BOOST_TEST(model.target()->rewritten);
	BOOST_TEST(!model.can_create());
	model.create();
	BOOST_TEST(calls.empty());
	model.rewrite_accepted() = true;
	BOOST_TEST(model.can_create());
	model.create();
	BOOST_TEST(calls.size() == 1u);
}

BOOST_AUTO_TEST_CASE(ReadingTheTargetAgain_ForgetsTheTick)
{
	next_target = rewritten_target();
	fill();
	model.rewrite_accepted() = true;
	model.refresh_target();
	BOOST_TEST(!model.rewrite_accepted());
	BOOST_TEST(!model.can_create());
}

BOOST_AUTO_TEST_CASE(ADeniedAnswer_ForgetsTheTarget_UntilItIsReadAgain)
{
	fill();
	model.create();
	finish(failure("denied", "the repository is not the one the request was allowed for"));
	BOOST_TEST(!model.target().has_value());
	BOOST_TEST(!model.can_create());
	model.refresh_target();
	BOOST_TEST(model.can_create());
}

BOOST_AUTO_TEST_SUITE_END()
