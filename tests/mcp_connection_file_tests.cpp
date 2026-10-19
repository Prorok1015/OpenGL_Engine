#include <boost/test/unit_test.hpp>
#include <snk/mcp/mcp_connection_file.h>
#include <boost/json/parse.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <system_error>

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

BOOST_AUTO_TEST_CASE(Write_CreatesTheDirectoryAndTheJson)
{
	temp_directory dir;
	mcp::connection_info info;
	info.port = 5150;
	info.token = "abc123";

	const fs::path snk_dir = dir.path / ".snk";
	BOOST_TEST(mcp::write_connection_file(snk_dir, info).empty());
	const fs::path file = mcp::connection_file_path(snk_dir);
	BOOST_REQUIRE(fs::exists(file));

	const boost::json::value parsed = boost::json::parse(read_all(file));
	BOOST_TEST(parsed.at("port").as_int64() == 5150);
	BOOST_TEST(std::string(parsed.at("token").as_string().c_str()) == "abc123");
	BOOST_TEST(std::string(parsed.at("url").as_string().c_str()) == "http://127.0.0.1:5150/mcp");
}

BOOST_AUTO_TEST_CASE(Write_ReplacesAnOldFile)
{
	temp_directory dir;
	mcp::connection_info info;
	info.port = 1;
	info.token = "old-token-that-is-longer-than-the-new-one";
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, info).empty());
	info.token = "new";
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, info).empty());
	const boost::json::value parsed = boost::json::parse(read_all(mcp::connection_file_path(dir.path)));
	BOOST_TEST(std::string(parsed.at("token").as_string().c_str()) == "new");
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(Write_IsReadableByTheUserOnly)
{
	temp_directory dir;
	mcp::connection_info info;
	info.token = "secret";
	// A file that already exists with wide permissions is narrowed too.
	fs::create_directories(dir.path);
	{
		std::ofstream out(mcp::connection_file_path(dir.path));
		out << "stale";
	}
	fs::permissions(mcp::connection_file_path(dir.path), fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);

	BOOST_REQUIRE(mcp::write_connection_file(dir.path, info).empty());
	const fs::perms perms = fs::status(mcp::connection_file_path(dir.path)).permissions();
	BOOST_TEST(((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none));
	BOOST_TEST(((perms & fs::perms::owner_read) != fs::perms::none));
}
#endif

BOOST_AUTO_TEST_CASE(Write_FailsWhenTheDirectoryCannotBeCreated)
{
	temp_directory dir;
	fs::create_directories(dir.path);
	// A file where the directory should go.
	{
		std::ofstream out(dir.path / "blocker");
		out << "x";
	}
	mcp::connection_info info;
	info.token = "t";
	BOOST_TEST(!mcp::write_connection_file(dir.path / "blocker" / "sub", info).empty());
}

BOOST_AUTO_TEST_CASE(Remove_DeletesTheFileAndToleratesAMissingOne)
{
	temp_directory dir;
	mcp::connection_info info;
	info.token = "t";
	BOOST_REQUIRE(mcp::write_connection_file(dir.path, info).empty());
	mcp::remove_connection_file(dir.path);
	BOOST_TEST(!fs::exists(mcp::connection_file_path(dir.path)));
	mcp::remove_connection_file(dir.path);
	mcp::remove_connection_file(dir.path / "never-created");
}

BOOST_AUTO_TEST_SUITE_END()
