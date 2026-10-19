#include <boost/test/unit_test.hpp>
#include <snk/mcp/mcp_connection_file.h>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

// The token and the connection file of snk_mcp_server (<project>/.snk/mcp.json).

namespace {

namespace fs = std::filesystem;

struct temp_directory
{
	fs::path path;

	temp_directory()
		: path(fs::temp_directory_path() / ("snk_mcp_test_" + mcp::generate_token().substr(0, 16)))
	{
	}
	~temp_directory()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

std::string read_all(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

mcp::connection_info make_info(std::uint16_t port, std::string token, std::int64_t pid)
{
	mcp::connection_info info;
	info.port = port;
	info.token = std::move(token);
	info.pid = pid;
	return info;
}

// No process has this id on any system the tests run on (Linux pid_max is below 2^22, Windows ids are 32-bit).
constexpr std::int64_t dead_pid = 2147483000;

}

BOOST_AUTO_TEST_SUITE(McpConnectionFileTests)

BOOST_AUTO_TEST_CASE(Token_Is64HexCharactersAndDiffersEveryTime)
{
	std::set<std::string> seen;
	for (int i = 0; i < 16; ++i) {
		const std::string token = mcp::generate_token();
		BOOST_TEST(token.size() == 64u);
		BOOST_TEST(std::all_of(token.begin(), token.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
		seen.insert(token);
	}
	BOOST_TEST(seen.size() == 16u);
}

BOOST_AUTO_TEST_CASE(Url_NamesTheLoopbackPortAndPath)
{
	mcp::connection_info info;
	info.port = 4242;
	BOOST_TEST(info.url() == "http://127.0.0.1:4242/mcp");
}

BOOST_AUTO_TEST_CASE(ProcessIds_ThisOneIsRunningAndAnImpossibleOneIsNot)
{
	BOOST_TEST(mcp::current_process_id() > 0);
	BOOST_TEST(mcp::process_is_running(mcp::current_process_id()));
	BOOST_TEST(!mcp::process_is_running(dead_pid));
	BOOST_TEST(!mcp::process_is_running(0));
	BOOST_TEST(!mcp::process_is_running(-5));
}

BOOST_AUTO_TEST_CASE(Write_CreatesTheDirectoryAndTheJson)
{
	temp_directory dir;
	const fs::path snk_dir = dir.path / ".snk";
	BOOST_TEST(mcp::write_connection_file(snk_dir, make_info(5150, "abc123", mcp::current_process_id())).empty());
	const fs::path file = mcp::connection_file_path(snk_dir);
	BOOST_REQUIRE(fs::exists(file));

	const boost::json::value parsed = boost::json::parse(read_all(file));
	BOOST_TEST(parsed.at("port").as_int64() == 5150);
	BOOST_TEST(std::string(parsed.at("token").as_string().c_str()) == "abc123");
	BOOST_TEST(std::string(parsed.at("url").as_string().c_str()) == "http://127.0.0.1:5150/mcp");
	BOOST_TEST(parsed.at("pid").as_int64() == mcp::current_process_id());

	const auto read = mcp::read_connection_file(snk_dir);
	BOOST_REQUIRE(read.has_value());
	BOOST_TEST(read->port == 5150u);
	BOOST_TEST(read->token == "abc123");
}

BOOST_AUTO_TEST_CASE(Write_ReplacesAnOldFileAndLeavesNoTemporaryFile)
{
	temp_directory dir;
	const std::int64_t me = mcp::current_process_id();
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, make_info(1, "old-token-that-is-longer-than-the-new-one", me)).empty());
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, make_info(2, "new", me)).empty());
	const auto read = mcp::read_connection_file(dir.path);
	BOOST_REQUIRE(read.has_value());
	BOOST_TEST(read->token == "new");

	int files = 0;
	for (const fs::directory_entry& entry : fs::directory_iterator(dir.path)) {
		(void)entry;
		++files;
	}
	BOOST_TEST(files == 1);
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(Write_IsReadableByTheUserOnly)
{
	temp_directory dir;
	const fs::path snk_dir = dir.path / ".snk";
	// A file that already exists with wide permissions is replaced by a narrow one.
	fs::create_directories(snk_dir);
	{
		std::ofstream out(mcp::connection_file_path(snk_dir));
		out << "stale";
	}
	fs::permissions(mcp::connection_file_path(snk_dir), fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);

	BOOST_REQUIRE(mcp::write_connection_file(snk_dir, make_info(1, "secret", mcp::current_process_id())).empty());
	const fs::perms perms = fs::status(mcp::connection_file_path(snk_dir)).permissions();
	BOOST_TEST(((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none));
	BOOST_TEST(((perms & fs::perms::owner_read) != fs::perms::none));
}

BOOST_AUTO_TEST_CASE(Write_ANewDirectoryIsOwnerOnly)
{
	temp_directory dir;
	const fs::path snk_dir = dir.path / ".snk";
	BOOST_REQUIRE(mcp::write_connection_file(snk_dir, make_info(1, "secret", mcp::current_process_id())).empty());
	const fs::perms perms = fs::status(snk_dir).permissions();
	BOOST_TEST(((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none));
}

BOOST_AUTO_TEST_CASE(Write_NeverFollowsALinkAtTheFile)
{
	temp_directory dir;
	fs::create_directories(dir.path);
	const fs::path victim = dir.path / "victim.txt";
	{
		std::ofstream out(victim);
		out << "precious";
	}
	const fs::path snk_dir = dir.path / ".snk";
	fs::create_directories(snk_dir);
	fs::create_symlink(victim, mcp::connection_file_path(snk_dir));

	BOOST_REQUIRE(mcp::write_connection_file(snk_dir, make_info(1, "secret", mcp::current_process_id())).empty());
	BOOST_TEST(read_all(victim) == "precious");
	BOOST_TEST(!fs::is_symlink(fs::symlink_status(mcp::connection_file_path(snk_dir))));
	BOOST_REQUIRE(mcp::read_connection_file(snk_dir).has_value());
}

BOOST_AUTO_TEST_CASE(Write_RefusesADirectoryThatIsALink)
{
	temp_directory dir;
	fs::create_directories(dir.path / "elsewhere");
	const fs::path snk_dir = dir.path / ".snk";
	fs::create_directory_symlink(dir.path / "elsewhere", snk_dir);

	const std::string failure = mcp::write_connection_file(snk_dir, make_info(1, "secret", mcp::current_process_id()));
	BOOST_TEST(!failure.empty());
	BOOST_TEST(!fs::exists(dir.path / "elsewhere" / "mcp.json"));
}

BOOST_AUTO_TEST_CASE(Write_RefusesAProjectAnotherRunningEditorServes)
{
	temp_directory dir;
	// The parent process is running and is not this one.
	const std::int64_t other = static_cast<std::int64_t>(::getppid());
	BOOST_REQUIRE(other > 0);
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, make_info(1, "theirs", other)).empty());

	const std::string failure = mcp::write_connection_file(dir.path, make_info(2, "mine", mcp::current_process_id()));
	BOOST_TEST(!failure.empty());
	const auto read = mcp::read_connection_file(dir.path);
	BOOST_REQUIRE(read.has_value());
	BOOST_TEST(read->token == "theirs");
}
#endif

BOOST_AUTO_TEST_CASE(Write_ReplacesTheFileOfADeadEditorAndOfAnUnreadableOne)
{
	temp_directory dir;
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, make_info(1, "dead", dead_pid)).empty());
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, make_info(2, "mine", mcp::current_process_id())).empty());
	BOOST_REQUIRE(mcp::read_connection_file(dir.path).has_value());
	BOOST_TEST(mcp::read_connection_file(dir.path)->token == "mine");

	{
		std::ofstream out(mcp::connection_file_path(dir.path), std::ios::trunc);
		out << "not json at all";
	}
	BOOST_TEST(!mcp::read_connection_file(dir.path).has_value());
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, make_info(3, "again", mcp::current_process_id())).empty());
	BOOST_TEST(mcp::read_connection_file(dir.path)->token == "again");
}

BOOST_AUTO_TEST_CASE(Write_FailsWhenTheDirectoryCannotBeCreated)
{
	temp_directory dir;
	fs::create_directories(dir.path);
	// A file where the directory should go.
	{
		std::ofstream out(dir.path / "blocker");
		out << "x";
	}
	BOOST_TEST(!mcp::write_connection_file(dir.path / "blocker" / "sub", make_info(1, "t", mcp::current_process_id())).empty());
}

BOOST_AUTO_TEST_CASE(Remove_DeletesOnlyTheFileOfThisEditor)
{
	temp_directory dir;
	const mcp::connection_info mine = make_info(1, "mine", mcp::current_process_id());
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, mine).empty());

	// Another editor took the project over since: its file stays.
	mcp::remove_connection_file(dir.path, make_info(1, "someone-else", mcp::current_process_id()));
	BOOST_TEST(fs::exists(mcp::connection_file_path(dir.path)));
	mcp::remove_connection_file(dir.path, make_info(1, "mine", mcp::current_process_id() + 1));
	BOOST_TEST(fs::exists(mcp::connection_file_path(dir.path)));

	mcp::remove_connection_file(dir.path, mine);
	BOOST_TEST(!fs::exists(mcp::connection_file_path(dir.path)));
	mcp::remove_connection_file(dir.path, mine);
	mcp::remove_connection_file(dir.path / "never-created", mine);
}

BOOST_AUTO_TEST_SUITE_END()
