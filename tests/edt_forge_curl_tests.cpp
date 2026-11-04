#include <boost/test/unit_test.hpp>
#include "edt_curl_transport.h"
#include <snk/platform/plt_secret.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

// The curl transport with a fake process runner (#458): what curl is given, where the token travels, what is left behind.

namespace {

	namespace fs = std::filesystem;
	using edt::forge::forge_error_code;
	using edt::forge::http_request;
	using edt::forge::http_response;

	const std::string canary = "ghp_CANARYCANARYCANARYCANARY0123456789";
	const std::string marker = "\n@@snk-http-status@@";

	std::string slurp(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		std::ostringstream out;
		out << file.rdbuf();
		return out.str();
	}

	bool contains(std::string_view text, std::string_view part)
	{
		return text.find(part) != std::string_view::npos;
	}

	// An empty 0700 folder under the OS temp folder, removed at the end.
	struct private_folder
	{
		fs::path dir;

		private_folder()
		{
			std::random_device entropy;
			std::error_code ec;
			dir = fs::temp_directory_path(ec) / ("snk_forge_test_" + std::to_string(entropy()) + std::to_string(entropy()));
			fs::create_directories(dir, ec);
#ifndef _WIN32
			fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, ec);
#endif
		}
		private_folder(const private_folder&) = delete;
		private_folder& operator=(const private_folder&) = delete;
		~private_folder()
		{
			std::error_code ec;
			fs::remove_all(dir, ec);
		}

		bool is_empty() const
		{
			std::error_code ec;
			return fs::is_empty(dir, ec);
		}
	};

	// Plays curl: remembers what it was given, looks into the files the way curl would, answers as told.
	struct fake_curl
	{
		std::string stdout_text = "{\"ok\":true}" + marker + "200\n";
		std::string stderr_text;
		platform::process_result result;
		bool throws = false;
		std::stop_source* stop_inside = nullptr;

		int calls = 0;
		fs::path program;
		std::vector<std::string> args;
		std::optional<std::vector<std::string>> environment;
		fs::path working_dir;
		fs::path config_path;
		std::string config_text;
		bool config_readable = false;
		fs::perms config_perms = fs::perms::none;
		fs::perms folder_perms = fs::perms::none;
		fs::path body_path;
		std::string body_text;

		edt::process_runner runner()
		{
			return [this](const fs::path& exe, const std::vector<std::string>& argv, const platform::process_options& options,
				const std::function<void(std::string_view)>& on_line, std::stop_token) {
				++calls;
				program = exe;
				args = argv;
				environment = options.environment;
				working_dir = options.working_dir;
				const auto flag = std::find(argv.begin(), argv.end(), "-K");
				if (flag != argv.end() && std::next(flag) != argv.end()) {
					config_path = *std::next(flag);
					config_text = slurp(config_path);
					config_readable = !config_text.empty();
					std::error_code ec;
					config_perms = fs::status(config_path, ec).permissions();
					folder_perms = fs::status(config_path.parent_path(), ec).permissions();
					const std::string key = "data-binary = \"@";
					const std::size_t at = config_text.find(key);
					if (at != std::string::npos) {
						const std::size_t from = at + key.size();
						body_path = config_text.substr(from, config_text.find('"', from) - from);
						body_text = slurp(body_path);
					}
				}
				if (throws) throw std::runtime_error("the runner failed");
				if (!stderr_text.empty() && options.on_stderr_line) options.on_stderr_line(stderr_text);
				if (stop_inside != nullptr) stop_inside->request_stop();
				if (!stdout_text.empty()) {
					// Handed over in pieces, the way a pipe would.
					for (std::size_t i = 0; i < stdout_text.size(); i += 7) on_line(std::string_view(stdout_text).substr(i, 7));
				}
				return result;
			};
		}
	};

	edt::forge::curl_options options_for(fake_curl& curl, const private_folder& folder)
	{
		edt::forge::curl_options options;
		options.curl = "curl-under-test";
		options.allowed_hosts = { "api.github.com" };
		options.run = curl.runner();
		options.scratch_dir = [dir = folder.dir] { return dir; };
		options.environment = std::vector<std::string>{ "PATH=/usr/bin", "HTTPS_PROXY=http://proxy.example:3128", "CURL_HOME=/somewhere" };
		return options;
	}

	http_request a_get(const platform::secret_text& token)
	{
		http_request request;
		request.method = "GET";
		request.url = "https://api.github.com/repos/octo/engine/pulls/1";
		request.headers = { { "Accept", "application/vnd.github+json" }, { "User-Agent", "snk-editor" } };
		request.bearer_token = &token;
		return request;
	}

	platform::secret_text make_token()
	{
		return platform::secret_text{ std::string(canary) };
	}

	bool has_flag(const std::vector<std::string>& args, std::string_view flag)
	{
		return std::find(args.begin(), args.end(), flag) != args.end();
	}

	std::string after_flag(const std::vector<std::string>& args, std::string_view flag)
	{
		const auto it = std::find(args.begin(), args.end(), flag);
		return it == args.end() || std::next(it) == args.end() ? std::string{} : *std::next(it);
	}

}

BOOST_AUTO_TEST_SUITE(EdtForgeCurl)

BOOST_AUTO_TEST_CASE(Get_RunsCurlWithTheHardenedArguments_AndReturnsTheBody)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();

	const http_response response = transport(a_get(token), {});

	BOOST_TEST(response.error_text == "");
	BOOST_TEST((response.status == 200));
	BOOST_TEST(response.body == "{\"ok\":true}");
	BOOST_REQUIRE(curl.calls == 1);
	BOOST_TEST(curl.program.string() == "curl-under-test");
	BOOST_REQUIRE(!curl.args.empty());
	BOOST_TEST(curl.args.front() == "-q");
	BOOST_TEST(has_flag(curl.args, "-K"));
	BOOST_TEST(after_flag(curl.args, "--proto") == "=https");
	BOOST_TEST(after_flag(curl.args, "--proto-redir") == "=https");
	BOOST_TEST(after_flag(curl.args, "--max-redirs") == "0");
	BOOST_TEST(after_flag(curl.args, "--max-time") == "30");
	BOOST_TEST(has_flag(curl.args, "--silent"));
	BOOST_TEST(has_flag(curl.args, "--show-error"));
	BOOST_TEST((after_flag(curl.args, "--max-filesize") == std::to_string(1024 * 1024)));
	BOOST_TEST(!has_flag(curl.args, "--fail-with-body"));
	BOOST_TEST(!has_flag(curl.args, "--location"));
	BOOST_TEST(!has_flag(curl.args, "-L"));
	BOOST_TEST(contains(after_flag(curl.args, "--write-out"), "%{http_code}"));
}

BOOST_AUTO_TEST_CASE(TheToken_IsNotInArgvOrEnvironment_ButInTheConfigFile)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();

	const http_response response = transport(a_get(token), {});

	BOOST_REQUIRE(response.error_text.empty());
	for (const std::string& arg : curl.args) BOOST_TEST(!contains(arg, canary));
	BOOST_REQUIRE(curl.environment.has_value());
	for (const std::string& entry : *curl.environment) BOOST_TEST(!contains(entry, canary));
	BOOST_TEST(curl.config_readable);
	BOOST_TEST(contains(curl.config_text, "header = \"Authorization: Bearer " + canary + "\""));
	BOOST_TEST(contains(curl.config_text, "url = \"https://api.github.com/repos/octo/engine/pulls/1\""));
	BOOST_TEST(contains(curl.config_text, "request = \"GET\""));
	BOOST_TEST(contains(curl.config_text, "header = \"Accept: application/vnd.github+json\""));
	BOOST_TEST(!contains(response.body, canary));
	BOOST_TEST(!contains(response.error_text, canary));
}

BOOST_AUTO_TEST_CASE(TheConfigFile_IsPrivate)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	BOOST_REQUIRE(transport(a_get(token), {}).error_text.empty());
#ifndef _WIN32
	BOOST_TEST(((curl.config_perms & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write)));
	BOOST_TEST(curl.config_path.parent_path() == folder.dir);
#else
	BOOST_TEST_MESSAGE("POSIX permissions are not checked on Windows; the file lives in the private temp folder");
#endif
}

BOOST_AUTO_TEST_CASE(TheEnvironment_KeepsTheProxy_AndDropsCurlHome)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	BOOST_REQUIRE(transport(a_get(token), {}).error_text.empty());
	BOOST_REQUIRE(curl.environment.has_value());
	const auto& env = *curl.environment;
	BOOST_TEST((std::find(env.begin(), env.end(), "HTTPS_PROXY=http://proxy.example:3128") != env.end()));
	BOOST_TEST((std::find(env.begin(), env.end(), "PATH=/usr/bin") != env.end()));
	BOOST_TEST(std::none_of(env.begin(), env.end(), [](const std::string& entry) { return entry.starts_with("CURL_HOME="); }));
}

BOOST_AUTO_TEST_CASE(TheFiles_AreGoneAfterTheCall)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	BOOST_REQUIRE(transport(a_get(token), {}).error_text.empty());
	BOOST_TEST(!fs::exists(curl.config_path));
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(Post_SendsTheBodyThroughAFile_AndRemovesBothFiles)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text = "{\"number\":1}" + marker + "201\n";
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	http_request request = a_get(token);
	request.method = "POST";
	request.url = "https://api.github.com/repos/octo/engine/pulls";
	request.body = "{\"title\":\"quote \\\" and \\\\ backslash\"}";

	const http_response response = transport(request, {});

	BOOST_TEST(response.error_text == "");
	BOOST_TEST((response.status == 201));
	BOOST_TEST(curl.body_text == request.body);
	BOOST_TEST(contains(curl.config_text, "request = \"POST\""));
	BOOST_TEST(!curl.body_path.empty());
	BOOST_TEST(!fs::exists(curl.body_path));
	BOOST_TEST(!fs::exists(curl.config_path));
	BOOST_TEST(folder.is_empty());
	for (const std::string& arg : curl.args) BOOST_TEST(!contains(arg, "quote"));
}

BOOST_AUTO_TEST_CASE(TheFiles_AreGoneWhenTheRunnerThrows)
{
	private_folder folder;
	fake_curl curl;
	curl.throws = true;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	http_request request = a_get(token);
	request.method = "POST";
	request.body = "{}";

	const http_response response = transport(request, {});

	BOOST_TEST(!response.error_text.empty());
	BOOST_TEST(!contains(response.error_text, canary));
	BOOST_TEST(!fs::exists(curl.config_path));
	BOOST_TEST(!fs::exists(curl.body_path));
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(TheFiles_AreGoneWhenTheRequestIsCancelled)
{
	private_folder folder;
	fake_curl curl;
	std::stop_source source;
	curl.stop_inside = &source;
	curl.result.cancelled = true;
	curl.stdout_text.clear();
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	http_request request = a_get(token);
	request.method = "POST";
	request.body = "{}";

	const http_response response = transport(request, source.get_token());

	BOOST_TEST(!response.error_text.empty());
	BOOST_TEST(!fs::exists(curl.config_path));
	BOOST_TEST(!fs::exists(curl.body_path));
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(AStopBeforeTheStart_RunsNothing)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	std::stop_source source;
	source.request_stop();
	const http_response response = transport(a_get(token), source.get_token());
	BOOST_TEST(!response.error_text.empty());
	BOOST_TEST(curl.calls == 0);
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(AHostOutsideTheAllowlist_IsRefusedBeforeCurlRuns)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	http_request request = a_get(token);
	request.url = "https://evil.example.com/repos/octo/engine/pulls/1";

	const http_response response = transport(request, {});

	BOOST_TEST((response.error_code == forge_error_code::host_not_allowed));
	BOOST_TEST(!response.error_text.empty());
	BOOST_TEST(curl.calls == 0);
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(TheAllowlist_IsComparedWithoutCase)
{
	private_folder folder;
	fake_curl curl;
	auto options = options_for(curl, folder);
	options.allowed_hosts = { "API.GitHub.com" };
	const auto transport = edt::forge::make_curl_transport(options);
	const auto token = make_token();
	http_request request = a_get(token);
	request.url = "https://Api.Github.Com/repos/octo/engine/pulls/1";
	BOOST_TEST(transport(request, {}).error_text.empty());
	BOOST_TEST(curl.calls == 1);
}

BOOST_AUTO_TEST_CASE(AnEmptyAllowlist_RefusesEverything)
{
	private_folder folder;
	fake_curl curl;
	auto options = options_for(curl, folder);
	options.allowed_hosts.clear();
	const auto transport = edt::forge::make_curl_transport(options);
	const auto token = make_token();
	BOOST_TEST((transport(a_get(token), {}).error_code == forge_error_code::host_not_allowed));
	BOOST_TEST(curl.calls == 0);
}

BOOST_AUTO_TEST_CASE(UnsafeUrls_AreRefusedBeforeCurlRuns)
{
	const std::vector<std::string> urls = {
		"http://api.github.com/repos/octo/engine/pulls/1",
		"ftp://api.github.com/x",
		"file:///etc/passwd",
		"https://user@api.github.com/x",
		"https://user:pw@api.github.com/x",
		"https://api.github.com:8443/x",
		"https://api.github.com/x y",
		"https://api.github.com/x\r\nHost: evil",
		"https://api.github.com/x\"\nurl = \"https://evil.example.com",
		"https://api.github.com\\@evil.example.com/",
		"",
		"api.github.com/x",
	};
	for (const std::string& url : urls) {
		private_folder folder;
		fake_curl curl;
		const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
		const auto token = make_token();
		http_request request = a_get(token);
		request.url = url;
		const http_response response = transport(request, {});
		BOOST_TEST_CONTEXT(url) {
			BOOST_TEST(!response.error_text.empty());
			BOOST_TEST(curl.calls == 0);
			BOOST_TEST(folder.is_empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(UnsafeHeadersAndTokens_AreRefusedBeforeCurlRuns)
{
	const auto token = make_token();
	std::vector<http_request> bad;
	{
		http_request request = a_get(token);
		request.headers.emplace_back("X-Bad", "a\r\nAuthorization: Bearer stolen");
		bad.push_back(request);
	}
	{
		http_request request = a_get(token);
		request.headers.emplace_back("Authorization", "Bearer other");
		bad.push_back(request);
	}
	{
		http_request request = a_get(token);
		request.headers.emplace_back("X Bad", "x");
		bad.push_back(request);
	}
	{
		http_request request = a_get(token);
		request.method = "GET /x HTTP/1.1\r\n";
		bad.push_back(request);
	}
	for (const http_request& request : bad) {
		private_folder folder;
		fake_curl curl;
		const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
		BOOST_TEST(!transport(request, {}).error_text.empty());
		BOOST_TEST(curl.calls == 0);
	}

	for (const std::string& text : { std::string("tok\nen"), std::string("tok en"), std::string("tok\"en\\"), std::string(), std::string("t\x01k") }) {
		private_folder folder;
		fake_curl curl;
		const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
		const platform::secret_text strange{ std::string(text) };
		http_request request = a_get(strange);
		const http_response response = transport(request, {});
		BOOST_TEST_CONTEXT(text.size()) {
			if (text == "tok\"en\\") {
				// Quote and backslash are legal in a token: escaped for the config file.
				BOOST_TEST(response.error_text.empty());
				BOOST_TEST(contains(curl.config_text, "Bearer tok\\\"en\\\\\""));
			} else {
				BOOST_TEST(!response.error_text.empty());
				BOOST_TEST(curl.calls == 0);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(ARedirect_IsAnError_WithoutTheLocation)
{
	for (const int status : { 301, 302, 307, 308 }) {
		private_folder folder;
		fake_curl curl;
		curl.stdout_text = "<html>moved</html>" + marker + std::to_string(status) + "\n";
		const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
		const auto token = make_token();
		const http_response response = transport(a_get(token), {});
		BOOST_TEST_CONTEXT(status) {
			BOOST_TEST(!response.error_text.empty());
			BOOST_TEST((response.status == 0));
			BOOST_TEST(response.body.empty());
			BOOST_TEST(contains(response.error_text, "redirect"));
		}
	}
}

BOOST_AUTO_TEST_CASE(ErrorStatuses_AreHandedOnWithTheirBody)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text = "{\"message\":\"Not Found\"}" + marker + "404\n";
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	const http_response response = transport(a_get(token), {});
	BOOST_TEST(response.error_text == "");
	BOOST_TEST((response.status == 404));
	BOOST_TEST(response.body == "{\"message\":\"Not Found\"}");
}

BOOST_AUTO_TEST_CASE(ABodyThatLooksLikeTheMarker_DoesNotForgeTheStatus)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text = "{\"x\":\"" + marker + "200\"}" + marker + "500\n";
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	const http_response response = transport(a_get(token), {});
	BOOST_TEST((response.status == 500));
}

BOOST_AUTO_TEST_CASE(MissingCurl_IsUnavailable_WithAClearMessage)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text.clear();
	curl.result.exit_code = -1;
	curl.result.error = "cannot start 'curl-under-test'";
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	const http_response response = transport(a_get(token), {});
	BOOST_TEST((response.error_code == forge_error_code::unavailable));
	BOOST_TEST(response.error_text == "curl was not found (install curl or set forge/curl)");
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(ANonZeroExit_IsANetworkError_WithTheStderrTail_WithoutTheToken)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text.clear();
	curl.result.exit_code = 7;
	curl.stderr_text = "curl: (7) Failed to connect; sent Authorization: Bearer " + canary + "\xE2\x80\xAE";
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();

	const http_response response = transport(a_get(token), {});

	BOOST_TEST((response.error_code == forge_error_code::network));
	BOOST_TEST(contains(response.error_text, "curl failed (exit 7)"));
	BOOST_TEST(contains(response.error_text, "Failed to connect"));
	BOOST_TEST(!contains(response.error_text, canary));
	BOOST_TEST(!contains(response.error_text, "\xE2\x80\xAE"));
	BOOST_TEST(response.error_text.size() <= edt::forge::k_max_error_message);
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(ATokenSplitAcrossTheStderrTailCut_IsStillScrubbed)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text.clear();
	curl.result.exit_code = 35;
	curl.stderr_text = std::string(3000, 'x') + canary;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	const http_response response = transport(a_get(token), {});
	BOOST_TEST(!contains(response.error_text, canary));
	BOOST_TEST(!contains(response.error_text, canary.substr(0, 12)));
}

BOOST_AUTO_TEST_CASE(ATimeout_IsReportedAsOne)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text.clear();
	curl.result.exit_code = 28;
	auto options = options_for(curl, folder);
	options.max_time_seconds = 5;
	const auto transport = edt::forge::make_curl_transport(options);
	const auto token = make_token();
	const http_response response = transport(a_get(token), {});
	BOOST_TEST(contains(response.error_text, "timed out"));
	BOOST_TEST(after_flag(curl.args, "--max-time") == "5");
}

BOOST_AUTO_TEST_CASE(AResponseOverTheLimit_IsAnError)
{
	private_folder folder;
	fake_curl curl;
	auto options = options_for(curl, folder);
	options.max_response_bytes = 64;
	curl.stdout_text = std::string(500, 'a') + marker + "200\n";
	curl.result.cancelled = true;
	const auto transport = edt::forge::make_curl_transport(options);
	const auto token = make_token();
	const http_response response = transport(a_get(token), {});
	BOOST_TEST((response.error_code == forge_error_code::invalid_response));
	BOOST_TEST(contains(response.error_text, "too large"));
	BOOST_TEST(after_flag(curl.args, "--max-filesize") == "64");
	BOOST_TEST(folder.is_empty());
}

BOOST_AUTO_TEST_CASE(ABodyJustOverTheLimit_ThatArrivesWhole_IsAnError)
{
	private_folder folder;
	fake_curl curl;
	auto options = options_for(curl, folder);
	options.max_response_bytes = 64;
	curl.stdout_text = std::string(65, 'a') + marker + "200\n";
	const auto transport = edt::forge::make_curl_transport(options);
	const auto token = make_token();
	BOOST_TEST((transport(a_get(token), {}).error_code == forge_error_code::invalid_response));
}

BOOST_AUTO_TEST_CASE(NoStatusLine_IsInvalidResponse)
{
	private_folder folder;
	fake_curl curl;
	curl.stdout_text = "{}";
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	BOOST_TEST((transport(a_get(token), {}).error_code == forge_error_code::invalid_response));
	curl.stdout_text = "{}" + marker + "abc\n";
	BOOST_TEST((transport(a_get(token), {}).error_code == forge_error_code::invalid_response));
	curl.stdout_text = "{}" + marker + "000\n";
	BOOST_TEST((transport(a_get(token), {}).error_code == forge_error_code::invalid_response));
}

BOOST_AUTO_TEST_CASE(NoToken_SendsNoAuthorizationHeader)
{
	private_folder folder;
	fake_curl curl;
	const auto transport = edt::forge::make_curl_transport(options_for(curl, folder));
	const auto token = make_token();
	http_request request = a_get(token);
	request.bearer_token = nullptr;
	BOOST_REQUIRE(transport(request, {}).error_text.empty());
	BOOST_TEST(!contains(curl.config_text, "Authorization"));
}

BOOST_AUTO_TEST_CASE(WithoutAScratchFolder_TheTransportMakesItsOwn_AndLeavesNothingBehind)
{
	fake_curl curl;
	edt::forge::curl_options options;
	options.allowed_hosts = { "api.github.com" };
	options.run = curl.runner();
	options.environment = std::vector<std::string>{ "PATH=/usr/bin" };
	fs::path folder;
	{
		const auto transport = edt::forge::make_curl_transport(options);
		const auto token = make_token();
		const http_response response = transport(a_get(token), {});
		BOOST_REQUIRE(response.error_text.empty());
		folder = curl.config_path.parent_path();
		BOOST_TEST(fs::is_directory(folder));
		BOOST_TEST(!fs::exists(curl.config_path));
		BOOST_TEST(!curl.program.empty());
#ifndef _WIN32
		BOOST_TEST(curl.program.string() == "curl");
		BOOST_TEST(((curl.folder_perms & fs::perms::all) == fs::perms::owner_all));
#endif
	}
	BOOST_TEST(!fs::exists(folder));
}

BOOST_AUTO_TEST_SUITE_END()
