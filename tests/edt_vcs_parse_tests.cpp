#include <boost/test/unit_test.hpp>
#include "edt_vcs_parse.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/string.hpp>
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// The pure git output parsers of the vcs service (#454) on fixtures. Non-ASCII text is spelled in escapes: MSVC
// reads the source in the local code page.

namespace {

namespace json = boost::json;
using namespace std::string_literals;

const std::string u_umlaut = "\xC3\xBC";                                // ü
const std::string cyrillic_file = "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB";   // файл

std::string text(const json::value& value)
{
	return std::string(value.as_string());
}

const edt::vcs::status_entry* entry_at(const edt::vcs::status_report& status, std::string_view path)
{
	const auto found = std::find_if(status.entries.begin(), status.entries.end(),
		[path](const edt::vcs::status_entry& entry) { return entry.path == path; });
	return found == status.entries.end() ? nullptr : &*found;
}

}

BOOST_AUTO_TEST_SUITE(EdtVcsParseTests)

BOOST_AUTO_TEST_CASE(Status_ReadsTheBranchHeaders)
{
	const std::string output = "# branch.oid 0123456789abcdef0123456789abcdef01234567\0"
		"# branch.head feature/x\0"
		"# branch.upstream origin/feature/x\0"
		"# branch.ab +3 -1\0"s;

	const auto status = edt::vcs::parse_status(output);

	BOOST_TEST(status.branch.head == "feature/x");
	BOOST_TEST(!status.branch.detached);
	BOOST_TEST(status.branch.oid == "0123456789abcdef0123456789abcdef01234567");
	BOOST_TEST(status.branch.upstream == "origin/feature/x");
	BOOST_REQUIRE(status.branch.ahead.has_value());
	BOOST_REQUIRE(status.branch.behind.has_value());
	BOOST_TEST(*status.branch.ahead == 3);
	BOOST_TEST(*status.branch.behind == 1);
	BOOST_TEST(status.entries.empty());
}

BOOST_AUTO_TEST_CASE(Status_DetachedAndUnborn)
{
	const auto detached = edt::vcs::parse_status("# branch.oid abc\0# branch.head (detached)\0"s);
	BOOST_TEST(detached.branch.detached);
	BOOST_TEST(detached.branch.head.empty());
	BOOST_TEST(!detached.branch.ahead.has_value());

	const auto unborn = edt::vcs::parse_status("# branch.oid (initial)\0# branch.head main\0"s);
	BOOST_TEST(unborn.branch.oid.empty());
	BOOST_TEST(unborn.branch.head == "main");
}

BOOST_AUTO_TEST_CASE(Status_ReadsEveryKindOfEntry_WithSpacesUtf8AndRenames)
{
	const std::string renamed_to = "docs/" + cyrillic_file + " new.md";
	const std::string untracked = "untracked " + u_umlaut + ".txt";
	const std::string output = "# branch.oid abc\0# branch.head main\0"s
		+ "1 .M N... 100644 100644 100644 aaaa bbbb src/main.cpp\0"s
		+ "1 A. N... 000000 100644 100644 0000 cccc dir with space/new file.txt\0"s
		+ "2 R. N... 100644 100644 100644 dddd dddd R100 " + renamed_to + "\0"s + "docs/old name.md\0"s
		+ "u UU N... 100644 100644 100644 100644 e1 e2 e3 conflict both.txt\0"s
		+ "? " + untracked + "\0"s
		+ "! build/\0"s;

	const auto status = edt::vcs::parse_status(output);

	BOOST_REQUIRE_EQUAL(status.entries.size(), 6u);
	const auto* modified = entry_at(status, "src/main.cpp");
	BOOST_REQUIRE(modified != nullptr);
	BOOST_TEST(modified->index == '.');
	BOOST_TEST(modified->worktree == 'M');
	BOOST_TEST((modified->kind == edt::vcs::entry_kind::ordinary));

	const auto* added = entry_at(status, "dir with space/new file.txt");
	BOOST_REQUIRE(added != nullptr);
	BOOST_TEST(added->index == 'A');

	const auto* renamed = entry_at(status, renamed_to);
	BOOST_REQUIRE(renamed != nullptr);
	BOOST_TEST((renamed->kind == edt::vcs::entry_kind::renamed));
	BOOST_TEST(renamed->orig_path == "docs/old name.md");
	BOOST_TEST(renamed->index == 'R');

	const auto* conflict = entry_at(status, "conflict both.txt");
	BOOST_REQUIRE(conflict != nullptr);
	BOOST_TEST((conflict->kind == edt::vcs::entry_kind::unmerged));
	BOOST_TEST(conflict->index == 'U');
	BOOST_TEST(conflict->worktree == 'U');

	const auto* new_file = entry_at(status, untracked);
	BOOST_REQUIRE(new_file != nullptr);
	BOOST_TEST((new_file->kind == edt::vcs::entry_kind::untracked));
	BOOST_TEST(new_file->index == '?');

	const auto* ignored = entry_at(status, "build/");
	BOOST_REQUIRE(ignored != nullptr);
	BOOST_TEST((ignored->kind == edt::vcs::entry_kind::ignored));
}

BOOST_AUTO_TEST_CASE(Status_SkipsRecordsItDoesNotKnow)
{
	const auto status = edt::vcs::parse_status("# branch.head main\0# stash 2\0x what\0" "1 .M short\0? ok.txt\0"s);
	BOOST_REQUIRE_EQUAL(status.entries.size(), 1u);
	BOOST_TEST(status.entries[0].path == "ok.txt");
}

BOOST_AUTO_TEST_CASE(Status_ToJson)
{
	const std::string output = "# branch.oid abc\0# branch.head main\0# branch.upstream origin/main\0# branch.ab +0 -2\0"
		"2 R. N... 100644 100644 100644 d d R100 b.txt\0a.txt\0"s;
	const json::value value = edt::vcs::to_json(edt::vcs::parse_status(output));
	const json::object& object = value.as_object();

	const json::object& branch = object.at("branch").as_object();
	BOOST_TEST(text(branch.at("head")) == "main");
	BOOST_TEST(text(branch.at("upstream")) == "origin/main");
	BOOST_TEST(branch.at("ahead").as_int64() == 0);
	BOOST_TEST(branch.at("behind").as_int64() == 2);
	BOOST_TEST(!object.at("clean").as_bool());
	const json::object& entry = object.at("entries").as_array().at(0).as_object();
	BOOST_TEST(text(entry.at("path")) == "b.txt");
	BOOST_TEST(text(entry.at("orig_path")) == "a.txt");
	BOOST_TEST(text(entry.at("kind")) == "renamed");
	BOOST_TEST(text(entry.at("index")) == "R");
	BOOST_TEST(text(entry.at("worktree")) == ".");

	const json::value empty = edt::vcs::to_json(edt::vcs::parse_status("# branch.head main\0"s));
	BOOST_TEST(empty.as_object().at("clean").as_bool());
	BOOST_TEST(!empty.as_object().at("branch").as_object().contains("upstream"));
}

BOOST_AUTO_TEST_CASE(Log_ReadsCommitsOfSixNulTerminatedFields)
{
	const std::string author = "J" + u_umlaut + "rgen";
	const std::string output = "1111\0" "2222 3333\0"s + author + "\0j@example.com\0" "1700000000\0Merge branch 'x'\0"s
		+ "2222\0" "3333\0Ann\0ann@example.com\0" "1600000000\0\0"s;

	const auto commits = edt::vcs::parse_log(output);

	BOOST_REQUIRE_EQUAL(commits.size(), 2u);
	BOOST_TEST(commits[0].hash == "1111");
	BOOST_TEST((commits[0].parents == std::vector<std::string>{ "2222", "3333" }));
	BOOST_TEST(commits[0].author == author);
	BOOST_TEST(commits[0].email == "j@example.com");
	BOOST_TEST(commits[0].time == 1700000000);
	BOOST_TEST(commits[0].subject == "Merge branch 'x'");
	BOOST_TEST(commits[1].subject.empty());
	BOOST_TEST((commits[1].parents == std::vector<std::string>{ "3333" }));

	const json::value value = edt::vcs::to_json(commits[0]);
	BOOST_TEST(text(value.as_object().at("author")) == author);
	BOOST_TEST(value.as_object().at("parents").as_array().size() == 2u);
	BOOST_TEST(value.as_object().at("time").as_int64() == 1700000000);
}

BOOST_AUTO_TEST_CASE(Log_RootCommitAndAnIncompleteTail)
{
	const auto commits = edt::vcs::parse_log("aaaa\0\0A\0a@x\0" "5\0root\0bbbb\0cccc\0"s);
	BOOST_REQUIRE_EQUAL(commits.size(), 1u);
	BOOST_TEST(commits[0].parents.empty());
	BOOST_TEST(edt::vcs::parse_log("").empty());
}

BOOST_AUTO_TEST_CASE(Refs_ReadsLocalAndRemoteBranches)
{
	const std::string output = "refs/heads/main\0" "aaaa\0refs/remotes/origin/main\0*\n"s
		+ "refs/heads/feature/" + u_umlaut + "\0" "bbbb\0\0 \r\n"s
		+ "refs/remotes/origin/HEAD\0" "aaaa\0\0 \n"s
		+ "refs/remotes/origin/main\0" "aaaa\0\0 \n"s
		+ "refs/tags/v1\0" "cccc\0\0 \n"s;

	const auto refs = edt::vcs::parse_refs(output);

	BOOST_REQUIRE_EQUAL(refs.size(), 3u);
	BOOST_TEST(refs[0].name == "main");
	BOOST_TEST(refs[0].full_name == "refs/heads/main");
	BOOST_TEST(refs[0].oid == "aaaa");
	BOOST_TEST(refs[0].upstream == "refs/remotes/origin/main");
	BOOST_TEST(refs[0].head);
	BOOST_TEST(!refs[0].remote);
	BOOST_TEST(refs[1].name == "feature/" + u_umlaut);
	BOOST_TEST(!refs[1].head);
	BOOST_TEST(refs[1].upstream.empty());
	BOOST_TEST(refs[2].name == "origin/main");
	BOOST_TEST(refs[2].remote);

	const json::value value = edt::vcs::to_json(refs[0]);
	BOOST_TEST(value.as_object().at("head").as_bool());
	BOOST_TEST(text(value.as_object().at("upstream")) == "refs/remotes/origin/main");
	BOOST_TEST(!edt::vcs::to_json(refs[1]).as_object().contains("upstream"));
}

BOOST_AUTO_TEST_CASE(ClassifyError_RecognisesAuthenticationFailures)
{
	for (const char* message : {
			"remote: Invalid username or password.\nfatal: Authentication failed for 'https://github.com/o/r.git/'",
			"fatal: could not read Username for 'https://github.com': terminal prompts disabled",
			"fatal: could not read Password for 'https://u@host': No such device or address",
			"git@github.com: Permission denied (publickey).\nfatal: Could not read from remote repository.",
			"Host key verification failed.\nfatal: Could not read from remote repository.",
			"fatal: unable to access 'https://host/r.git/': The requested URL returned error: 403" }) {
		BOOST_TEST_CONTEXT(message) {
			BOOST_TEST(std::string(edt::vcs::classify_error(message)) == "auth_required");
		}
	}
}

BOOST_AUTO_TEST_CASE(ClassifyError_OtherFailures)
{
	const auto kind = [](std::string_view message) { return std::string(edt::vcs::classify_error(message)); };
	BOOST_TEST(kind("fatal: not a git repository (or any of the parent directories): .git") == "not_a_repository");
	BOOST_TEST(kind("fatal: detected dubious ownership in repository at '/srv/r'") == "unsafe_repository");
	BOOST_TEST(kind("fatal: your current branch 'main' does not have any commits yet") == "no_commits");
	BOOST_TEST(kind("fatal: ambiguous argument 'nope': unknown revision or path not in the working tree.") == "bad_revision");
	BOOST_TEST(kind("fatal: unable to access 'https://h/r.git/': Could not resolve host: h") == "network");
	BOOST_TEST(kind("fatal: Unable to create '/r/.git/index.lock': File exists.") == "locked");
	BOOST_TEST(kind("error: something else") == "git_failed");
	BOOST_TEST(kind("") == "git_failed");
}

BOOST_AUTO_TEST_CASE(ValidUtf8_ReplacesWhatIsNotUtf8)
{
	BOOST_TEST(edt::vcs::valid_utf8("plain") == "plain");
	BOOST_TEST(edt::vcs::valid_utf8(u_umlaut + cyrillic_file) == u_umlaut + cyrillic_file);
	BOOST_TEST(edt::vcs::valid_utf8("a\xFC" "b") == "a\xEF\xBF\xBD" "b");          // Latin-1 ü
	BOOST_TEST(edt::vcs::valid_utf8("\xC3") == "\xEF\xBF\xBD");                    // cut short
	BOOST_TEST(edt::vcs::valid_utf8("\xED\xA0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"); // a surrogate
	BOOST_TEST(edt::vcs::valid_utf8("\xF0\x9F\x98\x80") == "\xF0\x9F\x98\x80");     // an emoji
}

BOOST_AUTO_TEST_CASE(Utf8Prefix_NeverEndsInsideACharacter)
{
	const std::string word = "a" + u_umlaut + "b";
	BOOST_TEST(edt::vcs::utf8_prefix(word, 10) == word);
	BOOST_TEST(edt::vcs::utf8_prefix(word, 3) == "a" + u_umlaut);
	BOOST_TEST(edt::vcs::utf8_prefix(word, 2) == "a");
	BOOST_TEST(edt::vcs::utf8_prefix(word, 0).empty());
}

BOOST_AUTO_TEST_CASE(ParseVersion)
{
	BOOST_TEST(edt::vcs::parse_version("git version 2.43.0\n") == "2.43.0");
	BOOST_TEST(edt::vcs::parse_version("git version 2.45.1.windows.1\r\n") == "2.45.1.windows.1");
	BOOST_TEST(edt::vcs::parse_version("  odd  ") == "odd");
}

BOOST_AUTO_TEST_SUITE_END()
