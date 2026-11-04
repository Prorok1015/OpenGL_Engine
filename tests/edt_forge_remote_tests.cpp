#include <boost/test/unit_test.hpp>
#include "edt_forge_types.h"
#include <string>
#include <string_view>
#include <vector>

// parse_forge_remote and the text helpers of the forge layer (#458). Non-ASCII text is spelled in escapes: MSVC reads the
// source in the local code page.

namespace {

	using edt::forge::forge_error_code;
	using edt::forge::forge_remote;
	using edt::forge::parse_forge_remote;

	struct accepted
	{
		std::string_view url;
		std::string_view host;
		std::string_view owner;
		std::string_view repo;
	};

	const accepted k_accepted[] = {
		{ "https://github.com/octo/engine", "github.com", "octo", "engine" },
		{ "https://github.com/octo/engine.git", "github.com", "octo", "engine" },
		{ "HTTPS://GitHub.COM/octo/engine.git", "github.com", "octo", "engine" },
		{ "git@github.com:octo/engine.git", "github.com", "octo", "engine" },
		{ "git@github.com:octo/engine", "github.com", "octo", "engine" },
		{ "git@GitHub.com:octo/my_repo-1.x", "github.com", "octo", "my_repo-1.x" },
		{ "ssh://git@github.com/octo/engine.git", "github.com", "octo", "engine" },
		{ "ssh://git@ghe.example.com/team-a/tool", "ghe.example.com", "team-a", "tool" },
		{ "https://github.com/octo/.github", "github.com", "octo", ".github" },
	};

	const std::string_view k_refused[] = {
		"",
		"http://github.com/octo/engine",
		"ftp://github.com/octo/engine",
		"file:///tmp/engine",
		"/home/user/engine",
		"../engine",
		"github.com/octo/engine",
		"https://user@github.com/octo/engine",
		"https://user:secret@github.com/octo/engine",
		"https://:secret@github.com/octo/engine",
		"https://github.com:8443/octo/engine",
		"ssh://git@github.com:22/octo/engine",
		"ssh://evil@github.com/octo/engine",
		"git@github.com:22/octo/engine",
		"https://github.com/octo/../engine",
		"https://github.com/../engine",
		"https://github.com/octo/en..gine",
		"https://github.com/octo/engine/extra",
		"https://github.com/octo/engine/",
		"https://github.com/octo",
		"https://github.com//engine",
		"https://github.com/octo/engine?x=1",
		"https://github.com/octo/engine#frag",
		"https://github.com/oc to/engine",
		"https://github.com/octo/en\"gine",
		"https://github.com/octo/en%67ine",
		"https://github.com/octo/engine\n",
		"https://github.com/o:cto/engine",
		"https://-github.com/octo/engine",
		"https://github..com/octo/engine",
		"git@github.com:octo/engine/extra",
		"git@github.com:../engine",
		"https://github.com/octo/.git",
		"https://[::1]/octo/engine",
	};

}

BOOST_AUTO_TEST_SUITE(EdtForgeRemote)

BOOST_AUTO_TEST_CASE(Parse_AcceptsTheSupportedForms)
{
	for (const accepted& row : k_accepted) {
		const auto result = parse_forge_remote(row.url);
		BOOST_TEST_CONTEXT(row.url) {
			BOOST_REQUIRE(result.ok());
			BOOST_TEST(result.value().host == row.host);
			BOOST_TEST(result.value().owner == row.owner);
			BOOST_TEST(result.value().repo == row.repo);
		}
	}
}

BOOST_AUTO_TEST_CASE(Parse_RefusesTheRest_AsValidation)
{
	for (const std::string_view url : k_refused) {
		const auto result = parse_forge_remote(url);
		BOOST_TEST_CONTEXT(url) {
			BOOST_TEST(!result.ok());
			if (!result.ok()) BOOST_TEST((result.error().code == forge_error_code::validation));
		}
	}
}

BOOST_AUTO_TEST_CASE(Parse_RefusesAHugeUrl)
{
	const std::string url = "https://github.com/octo/" + std::string(2000, 'a');
	BOOST_TEST(!parse_forge_remote(url).ok());
}

BOOST_AUTO_TEST_CASE(Parse_ErrorsNeverEchoTheUrl)
{
	const auto result = parse_forge_remote("https://user:hunter2@github.com/octo/engine");
	BOOST_REQUIRE(!result.ok());
	BOOST_TEST((result.error().message.find("hunter2") == std::string::npos));
}

BOOST_AUTO_TEST_CASE(Names_AreChecked)
{
	using edt::forge::is_safe_name;
	BOOST_TEST(is_safe_name("engine"));
	BOOST_TEST(is_safe_name("a.b_c-d"));
	BOOST_TEST(!is_safe_name(""));
	BOOST_TEST(!is_safe_name("."));
	BOOST_TEST(!is_safe_name(".."));
	BOOST_TEST(!is_safe_name("a..b"));
	BOOST_TEST(!is_safe_name("a/b"));
	BOOST_TEST(!is_safe_name("a b"));
	BOOST_TEST(!is_safe_name("a?b"));
	BOOST_TEST(!is_safe_name(std::string(101, 'a')));
}

BOOST_AUTO_TEST_CASE(Branches_AreChecked)
{
	using edt::forge::is_valid_branch_name;
	BOOST_TEST(is_valid_branch_name("main"));
	BOOST_TEST(is_valid_branch_name("feature/forge-1"));
	BOOST_TEST(is_valid_branch_name("release_1.2"));
	BOOST_TEST(!is_valid_branch_name(""));
	BOOST_TEST(!is_valid_branch_name("-x"));
	BOOST_TEST(!is_valid_branch_name("/x"));
	BOOST_TEST(!is_valid_branch_name("x/"));
	BOOST_TEST(!is_valid_branch_name("x."));
	BOOST_TEST(!is_valid_branch_name("a..b"));
	BOOST_TEST(!is_valid_branch_name("a//b"));
	BOOST_TEST(!is_valid_branch_name("a b"));
	BOOST_TEST(!is_valid_branch_name("a:b"));
	BOOST_TEST(!is_valid_branch_name("a@{b"));
	BOOST_TEST(!is_valid_branch_name("a\\b"));
	BOOST_TEST(!is_valid_branch_name("a.lock"));
	BOOST_TEST(!is_valid_branch_name("a\nb"));
	BOOST_TEST(!is_valid_branch_name(std::string(256, 'a')));
}

BOOST_AUTO_TEST_CASE(HttpsUrlHost_ReadsOnlyPlainHttpsUrls)
{
	using edt::forge::https_url_host;
	BOOST_TEST(https_url_host("https://API.GitHub.com/repos/a/b").value_or("") == "api.github.com");
	BOOST_TEST(https_url_host("https://api.github.com").value_or("") == "api.github.com");
	BOOST_TEST(!https_url_host("http://api.github.com/x"));
	BOOST_TEST(!https_url_host("https://u@api.github.com/x"));
	BOOST_TEST(!https_url_host("https://api.github.com:444/x"));
	BOOST_TEST(!https_url_host("https:///x"));
	BOOST_TEST(!https_url_host("https://api.github.com/a b"));
	BOOST_TEST(!https_url_host("https://api.github.com/a\r\nHost: x"));
	BOOST_TEST(!https_url_host("https://api.github.com\\@evil.com/"));
}

BOOST_AUTO_TEST_CASE(SanitizeText_DropsControlBidiAndInvisibleCharacters)
{
	using edt::forge::sanitize_text;
	const std::string bidi = "\xE2\x80\xAE";       // U+202E
	const std::string zero_width = "\xE2\x80\x8B"; // U+200B
	const std::string c1 = "\xC2\x85";             // U+0085
	const std::string cleaned = sanitize_text("a\x07" "b\r\nc" + bidi + "d" + zero_width + "e" + c1 + "f\x7F" "g", 100);
	BOOST_TEST(cleaned == "a b  c d e f g");
	BOOST_TEST(sanitize_text("ok \xC3\xBC", 100) == "ok \xC3\xBC");
}

BOOST_AUTO_TEST_CASE(SanitizeText_ReplacesInvalidUtf8)
{
	BOOST_TEST((edt::forge::sanitize_text("a\xFF" "b\xC3", 100) == "a\xEF\xBF\xBD" "b\xEF\xBF\xBD"));
	BOOST_TEST((edt::forge::sanitize_text("\xED\xA0\x80", 100) == "\xEF\xBF\xBD" "\xEF\xBF\xBD" "\xEF\xBF\xBD"));
	BOOST_TEST(edt::forge::is_valid_utf8("\xC3\xBC"));
	BOOST_TEST(!edt::forge::is_valid_utf8("\xC3"));
}

BOOST_AUTO_TEST_CASE(SanitizeText_CutsAtACharacterBoundary)
{
	const std::string text(50, 'a');
	const std::string cut = edt::forge::sanitize_text(text, 10);
	BOOST_TEST(cut.size() <= 10);
	BOOST_TEST(cut.ends_with("..."));

	const std::string umlauts = "\xC3\xBC\xC3\xBC\xC3\xBC\xC3\xBC\xC3\xBC\xC3\xBC";
	const std::string cut_umlauts = edt::forge::sanitize_text(umlauts, 8);
	BOOST_TEST(cut_umlauts.size() <= 8);
	BOOST_TEST(edt::forge::is_valid_utf8(cut_umlauts));
}

BOOST_AUTO_TEST_CASE(MakeForgeError_CapsTheMessage)
{
	const auto error = edt::forge::make_forge_error(forge_error_code::network, std::string(5000, 'x'));
	BOOST_TEST(error.message.size() <= edt::forge::k_max_error_message);
}

BOOST_AUTO_TEST_CASE(ScrubSecret_ReplacesEveryOccurrence)
{
	const platform::secret_text token{ std::string("tok_123") };
	std::string text = "a tok_123 b tok_123";
	edt::forge::scrub_secret(text, token);
	BOOST_TEST(text == "a <redacted> b <redacted>");
	std::string untouched = "abc";
	edt::forge::scrub_secret(untouched, platform::secret_text{});
	BOOST_TEST(untouched == "abc");
}

BOOST_AUTO_TEST_SUITE_END()
