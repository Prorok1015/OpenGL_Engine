#include <boost/test/unit_test.hpp>
#include "edt_forge_types.h"
#include "edt_github_provider.h"
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/string.hpp>
#include <snk/platform/plt_secret.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The GitHub provider on recorded responses and a fake transport (#458). Non-ASCII text is spelled in escapes.

namespace {

	namespace json = boost::json;
	using edt::forge::forge_error;
	using edt::forge::forge_error_code;
	using edt::forge::forge_remote;
	using edt::forge::http_request;
	using edt::forge::http_response;
	using edt::forge::pr_info;

	const std::string canary = "ghp_CANARYCANARYCANARYCANARY0123456789";

	// Answers with `next` and keeps what it was asked.
	struct fake_transport
	{
		http_response next;
		std::vector<http_request> requests;
		std::vector<std::string> tokens_seen;

		edt::forge::http_transport transport()
		{
			return [this](const http_request& request, std::stop_token) {
				requests.push_back(request);
				tokens_seen.push_back(request.bearer_token != nullptr ? request.bearer_token->reveal() : std::string{});
				return next;
			};
		}

		void answer(int status, std::string body)
		{
			next = http_response{};
			next.status = status;
			next.body = std::move(body);
		}
	};

	forge_remote octo()
	{
		return forge_remote{ "github.com", "octo", "engine" };
	}

	bool has_canary(std::string_view text)
	{
		return text.find(canary) != std::string_view::npos;
	}

	void check_clean(const forge_error& error)
	{
		BOOST_TEST(!has_canary(error.message));
		BOOST_TEST(error.message.size() <= edt::forge::k_max_error_message);
	}

	void check_clean(const pr_info& info)
	{
		BOOST_TEST(!has_canary(info.title));
		BOOST_TEST(!has_canary(info.state));
		BOOST_TEST(!has_canary(info.head));
		BOOST_TEST(!has_canary(info.base));
		BOOST_TEST(!has_canary(info.url));
	}

	// The token is in bearer_token and nowhere else of the request.
	void check_request_carries_token_only_as_bearer(const fake_transport& fake, const platform::secret_text& token)
	{
		for (std::size_t i = 0; i < fake.requests.size(); ++i) {
			const http_request& request = fake.requests[i];
			BOOST_TEST(request.bearer_token == &token);
			BOOST_TEST(fake.tokens_seen[i] == canary);
			BOOST_TEST(!has_canary(request.method));
			BOOST_TEST(!has_canary(request.url));
			BOOST_TEST(!has_canary(request.body));
			for (const auto& [name, value] : request.headers) {
				BOOST_TEST(!has_canary(name));
				BOOST_TEST(!has_canary(value));
			}
		}
	}

	std::string header(const http_request& request, std::string_view name)
	{
		for (const auto& [key, value] : request.headers) {
			if (key == name) return value;
		}
		return "<missing>";
	}

	platform::secret_text make_token()
	{
		return platform::secret_text{ std::string(canary) };
	}

	const std::string pr_created = R"json({
		"number": 42, "title": "Add the forge layer", "state": "open", "draft": true, "merged": false,
		"html_url": "https://github.com/octo/engine/pull/42",
		"head": { "ref": "feature/forge", "sha": "abc" }, "base": { "ref": "main", "sha": "def" },
		"user": { "login": "octo" }
	})json";

	edt::forge::create_pr_request a_request()
	{
		edt::forge::create_pr_request request;
		request.base = "main";
		request.head = "feature/forge";
		request.title = "Add the forge layer";
		request.body = "Details\nsecond line";
		return request;
	}

}

BOOST_AUTO_TEST_SUITE(EdtForgeGithub)

BOOST_AUTO_TEST_CASE(CreatePr_Success_SendsTheDocumentedRequest)
{
	fake_transport fake;
	fake.answer(201, pr_created);
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();

	const auto result = provider.create_pr(octo(), a_request(), token);

	BOOST_REQUIRE(result.ok());
	BOOST_TEST(result.value().number == 42);
	BOOST_TEST(result.value().title == "Add the forge layer");
	BOOST_TEST(result.value().state == "open");
	BOOST_TEST(result.value().draft);
	BOOST_TEST(result.value().head == "feature/forge");
	BOOST_TEST(result.value().base == "main");
	BOOST_TEST(result.value().url == "https://github.com/octo/engine/pull/42");
	check_clean(result.value());

	BOOST_REQUIRE(fake.requests.size() == 1);
	const http_request& request = fake.requests[0];
	BOOST_TEST(request.method == "POST");
	BOOST_TEST(request.url == "https://api.github.com/repos/octo/engine/pulls");
	BOOST_TEST(header(request, "Accept") == "application/vnd.github+json");
	BOOST_TEST(header(request, "X-GitHub-Api-Version") == "2022-11-28");
	BOOST_TEST(header(request, "User-Agent") == "snk-editor");
	BOOST_TEST(header(request, "Content-Type") == "application/json");
	const json::object payload = json::parse(request.body).as_object();
	BOOST_TEST(std::string(payload.at("title").as_string()) == "Add the forge layer");
	BOOST_TEST(std::string(payload.at("head").as_string()) == "feature/forge");
	BOOST_TEST(std::string(payload.at("base").as_string()) == "main");
	BOOST_TEST(std::string(payload.at("body").as_string()) == "Details\nsecond line");
	BOOST_TEST(payload.at("draft").as_bool());
	check_request_carries_token_only_as_bearer(fake, token);
}

BOOST_AUTO_TEST_CASE(CreatePr_SendsDraftFalseWhenAsked)
{
	fake_transport fake;
	fake.answer(201, pr_created);
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	auto request = a_request();
	request.draft = false;
	BOOST_REQUIRE(provider.create_pr(octo(), request, token).ok());
	BOOST_TEST(!json::parse(fake.requests.at(0).body).as_object().at("draft").as_bool());
}

BOOST_AUTO_TEST_CASE(CreatePr_AlreadyExists_IsValidationWithGithubsMessage)
{
	fake_transport fake;
	fake.answer(422, R"json({"message":"Validation Failed","errors":[{"resource":"PullRequest","code":"custom","message":"A pull request already exists for octo:feature/forge."}]})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();

	const auto result = provider.create_pr(octo(), a_request(), token);

	BOOST_REQUIRE(!result.ok());
	BOOST_TEST((result.error().code == forge_error_code::validation));
	BOOST_TEST(result.error().message.find("A pull request already exists") != std::string::npos);
	check_clean(result.error());
}

BOOST_AUTO_TEST_CASE(Statuses_MapToCodes)
{
	struct row
	{
		int status;
		std::string body;
		forge_error_code code;
	};
	const std::vector<row> rows = {
		{ 401, R"json({"message":"Bad credentials"})json", forge_error_code::unauthorized },
		{ 403, R"json({"message":"Resource not accessible by personal access token"})json", forge_error_code::forbidden },
		{ 403, R"json({"message":"API rate limit exceeded for user ID 1."})json", forge_error_code::rate_limited },
		{ 403, R"json({"message":"You have exceeded a secondary rate limit."})json", forge_error_code::rate_limited },
		{ 429, "", forge_error_code::rate_limited },
		{ 404, R"json({"message":"Not Found"})json", forge_error_code::not_found },
		{ 422, R"json({"message":"Validation Failed"})json", forge_error_code::validation },
		{ 500, "<html>boom</html>", forge_error_code::network },
		{ 502, "", forge_error_code::network },
		{ 418, "", forge_error_code::invalid_response },
	};
	for (const row& r : rows) {
		fake_transport fake;
		fake.answer(r.status, r.body);
		edt::forge::github_provider provider(fake.transport());
		const auto token = make_token();
		const auto result = provider.pr_status(octo(), 7, token);
		BOOST_TEST_CONTEXT("HTTP " << r.status << " " << r.body) {
			BOOST_REQUIRE(!result.ok());
			BOOST_TEST((result.error().code == r.code));
			BOOST_TEST(!result.error().message.empty());
			check_clean(result.error());
		}
	}
}

BOOST_AUTO_TEST_CASE(Errors_ScrubTheTokenAndTheServersControlCharacters)
{
	fake_transport fake;
	// The server echoes the token, rings the bell and flips the text direction.
	fake.answer(401, R"json({"message":"Bad credentials ghp_CANARYCANARYCANARYCANARY0123456789 \u0007 ‮ evil ​"})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();

	const auto result = provider.pr_status(octo(), 7, token);

	BOOST_REQUIRE(!result.ok());
	check_clean(result.error());
	BOOST_TEST(result.error().message.find('\x07') == std::string::npos);
	BOOST_TEST(result.error().message.find("\xE2\x80\xAE") == std::string::npos);
	BOOST_TEST(result.error().message.find("\xE2\x80\x8B") == std::string::npos);
	BOOST_TEST(result.error().message.find("Bad credentials") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Errors_CapAHugeServerMessage)
{
	fake_transport fake;
	fake.answer(422, R"json({"message":")json" + std::string(100000, 'x') + R"json("})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.create_pr(octo(), a_request(), token);
	BOOST_REQUIRE(!result.ok());
	BOOST_TEST(result.error().message.size() <= edt::forge::k_max_error_message);
}

BOOST_AUTO_TEST_CASE(TransportFailures_PassThroughWithoutTheToken)
{
	fake_transport fake;
	fake.next.error_text = "curl failed (exit 6): could not resolve " + canary;
	fake.next.error_code = forge_error_code::unavailable;
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.pr_status(octo(), 7, token);
	BOOST_REQUIRE(!result.ok());
	BOOST_TEST((result.error().code == forge_error_code::unavailable));
	check_clean(result.error());
}

BOOST_AUTO_TEST_CASE(PrStatus_Open_AndTheRequest)
{
	fake_transport fake;
	fake.answer(200, pr_created);
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.pr_status(octo(), 42, token);
	BOOST_REQUIRE(result.ok());
	BOOST_TEST(result.value().state == "open");
	BOOST_REQUIRE(fake.requests.size() == 1);
	BOOST_TEST(fake.requests[0].method == "GET");
	BOOST_TEST(fake.requests[0].url == "https://api.github.com/repos/octo/engine/pulls/42");
	BOOST_TEST(fake.requests[0].body.empty());
	check_request_carries_token_only_as_bearer(fake, token);
}

BOOST_AUTO_TEST_CASE(PrStatus_Merged)
{
	fake_transport fake;
	fake.answer(200, R"json({"number":9,"title":"Done","state":"closed","draft":false,"merged":true,"merged_at":"2026-11-01T10:00:00Z","html_url":"https://github.com/octo/engine/pull/9","head":{"ref":"a"},"base":{"ref":"main"}})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.pr_status(octo(), 9, token);
	BOOST_REQUIRE(result.ok());
	BOOST_TEST(result.value().state == "merged");
	BOOST_TEST(!result.value().draft);
}

BOOST_AUTO_TEST_CASE(PrStatus_ClosedWithoutMerge)
{
	fake_transport fake;
	fake.answer(200, R"json({"number":9,"title":"Nope","state":"closed","merged":false,"merged_at":null,"head":{"ref":"a"},"base":{"ref":"main"}})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.pr_status(octo(), 9, token);
	BOOST_REQUIRE(result.ok());
	BOOST_TEST(result.value().state == "closed");
	BOOST_TEST(result.value().url.empty());
}

BOOST_AUTO_TEST_CASE(ListPrs_ReadsTheItems_AndBuildsTheQuery)
{
	fake_transport fake;
	fake.answer(200, R"json([
		{"number":2,"title":"Second","state":"open","draft":true,"html_url":"https://github.com/octo/engine/pull/2","head":{"ref":"b"},"base":{"ref":"main"}},
		{"number":1,"title":"First","state":"open","draft":false,"merged_at":null,"html_url":"https://github.com/octo/engine/pull/1","head":{"ref":"a"},"base":{"ref":"main"}}
	])json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	edt::forge::list_prs_filter filter;
	filter.head = "feature/forge";
	filter.base = "main";
	filter.limit = 50;

	const auto result = provider.list_prs(octo(), filter, token);

	BOOST_REQUIRE(result.ok());
	BOOST_REQUIRE(result.value().size() == 2);
	BOOST_TEST(result.value()[0].number == 2);
	BOOST_TEST(result.value()[0].draft);
	BOOST_TEST(result.value()[1].number == 1);
	BOOST_TEST(!result.value()[1].draft);
	for (const pr_info& info : result.value()) check_clean(info);
	BOOST_REQUIRE(fake.requests.size() == 1);
	BOOST_TEST(fake.requests[0].method == "GET");
	BOOST_TEST(fake.requests[0].url
		== "https://api.github.com/repos/octo/engine/pulls?state=open&per_page=50&page=1&head=octo%3Afeature%2Fforge&base=main");
	check_request_carries_token_only_as_bearer(fake, token);
}

BOOST_AUTO_TEST_CASE(ListPrs_HeadWithAnOwner_KeepsIt_AndTheLimitIsClamped)
{
	fake_transport fake;
	fake.answer(200, "[]");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	edt::forge::list_prs_filter filter;
	filter.state = "all";
	filter.head = "fork-owner:topic";
	filter.limit = 5000;
	const auto result = provider.list_prs(octo(), filter, token);
	BOOST_REQUIRE(result.ok());
	BOOST_TEST(result.value().empty());
	BOOST_TEST(fake.requests.at(0).url
		== "https://api.github.com/repos/octo/engine/pulls?state=all&per_page=100&page=1&head=fork-owner%3Atopic");
}

BOOST_AUTO_TEST_CASE(ListPrs_RefusesAFilterThatCouldChangeTheQuery)
{
	const auto token = make_token();
	for (const std::string value : { std::string("a b"), std::string("a\nb"), std::string("..") }) {
		fake_transport fake;
		fake.answer(200, "[]");
		edt::forge::github_provider provider(fake.transport());
		edt::forge::list_prs_filter by_base;
		by_base.base = value;
		edt::forge::list_prs_filter by_head;
		by_head.head = value;
		BOOST_TEST(!provider.list_prs(octo(), by_base, token).ok());
		BOOST_TEST(!provider.list_prs(octo(), by_head, token).ok());
		BOOST_TEST(fake.requests.empty());
	}
	fake_transport fake;
	edt::forge::github_provider provider(fake.transport());
	edt::forge::list_prs_filter bad_state;
	bad_state.state = "open&x=1";
	BOOST_TEST(!provider.list_prs(octo(), bad_state, token).ok());
	BOOST_TEST(fake.requests.empty());
}

BOOST_AUTO_TEST_CASE(ListPrs_PercentEncodesQueryValues)
{
	fake_transport fake;
	fake.answer(200, "[]");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	edt::forge::list_prs_filter filter;
	filter.base = "a&state=all#x";
	BOOST_REQUIRE(provider.list_prs(octo(), filter, token).ok());
	BOOST_TEST(fake.requests.at(0).url
		== "https://api.github.com/repos/octo/engine/pulls?state=open&per_page=30&page=1&base=a%26state%3Dall%23x");
}

BOOST_AUTO_TEST_CASE(Responses_MalformedJson_IsInvalidResponse)
{
	const auto token = make_token();
	for (const std::string body : { std::string("{\"number\":"), std::string(""), std::string("not json"), std::string("{} trailing") }) {
		fake_transport fake;
		fake.answer(200, body);
		edt::forge::github_provider provider(fake.transport());
		const auto result = provider.pr_status(octo(), 1, token);
		BOOST_TEST_CONTEXT(body) {
			BOOST_REQUIRE(!result.ok());
			BOOST_TEST((result.error().code == forge_error_code::invalid_response));
		}
	}
}

BOOST_AUTO_TEST_CASE(Responses_NotAnObject_IsInvalidResponse)
{
	const auto token = make_token();
	for (const std::string body : { std::string("[1,2]"), std::string("42"), std::string("\"text\""), std::string("null") }) {
		fake_transport fake;
		fake.answer(201, body);
		edt::forge::github_provider provider(fake.transport());
		const auto result = provider.create_pr(octo(), a_request(), token);
		BOOST_TEST_CONTEXT(body) {
			BOOST_REQUIRE(!result.ok());
			BOOST_TEST((result.error().code == forge_error_code::invalid_response));
		}
	}
}

BOOST_AUTO_TEST_CASE(Responses_AnObjectWhereAListIsExpected_IsInvalidResponse)
{
	fake_transport fake;
	fake.answer(200, pr_created);
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.list_prs(octo(), {}, token);
	BOOST_REQUIRE(!result.ok());
	BOOST_TEST((result.error().code == forge_error_code::invalid_response));
}

BOOST_AUTO_TEST_CASE(Responses_MissingFields_AreInvalidResponse)
{
	const auto token = make_token();
	for (const std::string body : {
			std::string(R"json({"title":"x","state":"open","head":{"ref":"a"},"base":{"ref":"b"}})json"),
			std::string(R"json({"number":1,"state":"open","head":{"ref":"a"},"base":{"ref":"b"}})json"),
			std::string(R"json({"number":1,"title":"x","state":"weird","head":{"ref":"a"},"base":{"ref":"b"}})json"),
			std::string(R"json({"number":1,"title":"x","state":"open","base":{"ref":"b"}})json"),
			std::string(R"json({"number":"1","title":"x","state":"open","head":{"ref":"a"},"base":{"ref":"b"}})json"),
			std::string(R"json({"number":-3,"title":"x","state":"open","head":{"ref":"a"},"base":{"ref":"b"}})json") }) {
		fake_transport fake;
		fake.answer(200, body);
		edt::forge::github_provider provider(fake.transport());
		const auto result = provider.pr_status(octo(), 1, token);
		BOOST_TEST_CONTEXT(body) {
			BOOST_REQUIRE(!result.ok());
			BOOST_TEST((result.error().code == forge_error_code::invalid_response));
		}
	}
}

BOOST_AUTO_TEST_CASE(Responses_TooDeeplyNested_IsInvalidResponse)
{
	fake_transport fake;
	fake.answer(200, std::string(200, '[') + std::string(200, ']'));
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.list_prs(octo(), {}, token);
	BOOST_REQUIRE(!result.ok());
	BOOST_TEST((result.error().code == forge_error_code::invalid_response));
}

BOOST_AUTO_TEST_CASE(Responses_Oversized_IsInvalidResponse)
{
	fake_transport fake;
	fake.answer(200, R"json({"number":1,"title":")json" + std::string(2 * 1024 * 1024, 'x') + R"json("})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.pr_status(octo(), 1, token);
	BOOST_REQUIRE(!result.ok());
	BOOST_TEST((result.error().code == forge_error_code::invalid_response));
}

BOOST_AUTO_TEST_CASE(HtmlUrl_OfAnotherHost_IsDropped)
{
	const auto token = make_token();
	for (const std::string url : { std::string("https://evil.example.com/octo/engine/pull/1"), std::string("http://github.com/octo/engine/pull/1"),
			std::string("https://github.com.evil.example/x"), std::string("https://user@github.com/x"), std::string("javascript:alert(1)") }) {
		fake_transport fake;
		fake.answer(200, R"json({"number":1,"title":"x","state":"open","html_url":")json" + url + R"json(","head":{"ref":"a"},"base":{"ref":"b"}})json");
		edt::forge::github_provider provider(fake.transport());
		const auto result = provider.pr_status(octo(), 1, token);
		BOOST_TEST_CONTEXT(url) {
			BOOST_REQUIRE(result.ok());
			BOOST_TEST(result.value().url.empty());
			BOOST_TEST(result.value().number == 1);
		}
	}
}

BOOST_AUTO_TEST_CASE(Fields_FromTheServer_AreSanitized)
{
	fake_transport fake;
	fake.answer(200, R"json({"number":1,"title":"Fix‮ txt.exe\u0007 ​ ghp_CANARYCANARYCANARYCANARY0123456789","state":"open","head":{"ref":"a\nb"},"base":{"ref":"main"}})json");
	edt::forge::github_provider provider(fake.transport());
	const auto token = make_token();
	const auto result = provider.pr_status(octo(), 1, token);
	BOOST_REQUIRE(result.ok());
	check_clean(result.value());
	BOOST_TEST(result.value().title.find("\xE2\x80\xAE") == std::string::npos);
	BOOST_TEST(result.value().title.find('\x07') == std::string::npos);
	BOOST_TEST(result.value().head.find('\n') == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Hosts_OnlyGithubCom_UnlessEnterpriseIsConfigured)
{
	const auto token = make_token();
	{
		fake_transport fake;
		fake.answer(200, pr_created);
		edt::forge::github_provider provider(fake.transport());
		const forge_remote elsewhere{ "gitlab.com", "octo", "engine" };
		const auto result = provider.pr_status(elsewhere, 1, token);
		BOOST_REQUIRE(!result.ok());
		BOOST_TEST((result.error().code == forge_error_code::host_not_allowed));
		BOOST_TEST(fake.requests.empty());
	}
	{
		fake_transport fake;
		fake.answer(200, R"json({"number":3,"title":"E","state":"open","html_url":"https://ghe.example.com/octo/engine/pull/3","head":{"ref":"a"},"base":{"ref":"b"}})json");
		edt::forge::github_options options;
		options.api_base = "https://ghe.example.com/api/v3/";
		options.web_host = "ghe.example.com";
		edt::forge::github_provider provider(fake.transport(), options);
		const forge_remote enterprise{ "ghe.example.com", "octo", "engine" };
		const auto result = provider.pr_status(enterprise, 3, token);
		BOOST_REQUIRE(result.ok());
		BOOST_TEST(result.value().url == "https://ghe.example.com/octo/engine/pull/3");
		BOOST_REQUIRE(fake.requests.size() == 1);
		BOOST_TEST(fake.requests[0].url == "https://ghe.example.com/api/v3/repos/octo/engine/pulls/3");

		const forge_remote other{ "other.example.com", "octo", "engine" };
		BOOST_TEST(!provider.pr_status(other, 3, token).ok());
		BOOST_TEST(fake.requests.size() == 1);
	}
	{
		fake_transport fake;
		fake.answer(200, pr_created);
		edt::forge::github_options options;
		options.api_base = "http://ghe.example.com/api/v3";
		options.web_host = "ghe.example.com";
		edt::forge::github_provider provider(fake.transport(), options);
		const forge_remote enterprise{ "ghe.example.com", "octo", "engine" };
		const auto result = provider.pr_status(enterprise, 3, token);
		BOOST_REQUIRE(!result.ok());
		BOOST_TEST((result.error().code == forge_error_code::host_not_allowed));
		BOOST_TEST(fake.requests.empty());
	}
}

BOOST_AUTO_TEST_CASE(Inputs_AreValidatedBeforeAnyRequest)
{
	const auto token = make_token();
	fake_transport fake;
	fake.answer(201, pr_created);
	edt::forge::github_provider provider(fake.transport());

	BOOST_TEST(!provider.pr_status(forge_remote{ "github.com", "a/b", "engine" }, 1, token).ok());
	BOOST_TEST(!provider.pr_status(forge_remote{ "github.com", "octo", ".." }, 1, token).ok());
	BOOST_TEST(!provider.pr_status(forge_remote{ "github.com", "octo", "en?x=1" }, 1, token).ok());
	BOOST_TEST(!provider.pr_status(octo(), 0, token).ok());
	BOOST_TEST(!provider.pr_status(octo(), -5, token).ok());

	auto bad_head = a_request();
	bad_head.head = "a..b";
	BOOST_TEST(!provider.create_pr(octo(), bad_head, token).ok());
	auto bad_base = a_request();
	bad_base.base = "-main";
	BOOST_TEST(!provider.create_pr(octo(), bad_base, token).ok());
	auto no_title = a_request();
	no_title.title.clear();
	BOOST_TEST(!provider.create_pr(octo(), no_title, token).ok());
	auto bad_utf8 = a_request();
	bad_utf8.body = "\xFF\xFE";
	BOOST_TEST(!provider.create_pr(octo(), bad_utf8, token).ok());
	auto long_title = a_request();
	long_title.title = std::string(300, 't');
	BOOST_TEST(!provider.create_pr(octo(), long_title, token).ok());

	BOOST_TEST(fake.requests.empty());
}

BOOST_AUTO_TEST_CASE(Name_IsGithub)
{
	fake_transport fake;
	edt::forge::github_provider provider(fake.transport());
	BOOST_TEST(provider.name() == "github");
}

BOOST_AUTO_TEST_SUITE_END()
