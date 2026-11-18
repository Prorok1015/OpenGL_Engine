#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_beneath.h>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace {

platform::beneath::read_request request_for(const fs::path& root, const fs::path& relative)
{
	platform::beneath::read_request request;
	request.root = fs::weakly_canonical(root);
	request.folder = relative.parent_path();
	request.leaf = relative.filename();
	request.max_bytes = 1u << 30;
	return request;
}

}

BOOST_AUTO_TEST_SUITE(PltBeneathStream)

// #760: a file larger than one chunk comes in order and whole, as read_beneath gives it.
BOOST_AUTO_TEST_CASE(a_stream_gives_the_same_bytes_as_a_read)
{
	test_util::temp_dir dir("beneath_stream");
	std::string text;
	for (int i = 0; text.size() < 200u * 1024u; ++i) {
		text += std::to_string(i) + ",";
	}
	dir.write("a/big.txt", text);

	std::string streamed;
	std::size_t chunks = 0;
	const auto result = platform::beneath::read_beneath_stream(request_for(dir.path(), "a/big.txt"),
		[&](std::span<const std::byte> chunk) {
			streamed.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
			++chunks;
			return true;
		});
	BOOST_TEST((result.status == platform::beneath::read_status::ok));
	BOOST_TEST(streamed == text);
	BOOST_TEST(chunks > 1u);

	std::vector<std::byte> whole;
	BOOST_TEST((platform::beneath::read_beneath(request_for(dir.path(), "a/big.txt"), whole).status == platform::beneath::read_status::ok));
	BOOST_TEST(whole.size() == text.size());
}

BOOST_AUTO_TEST_CASE(the_sink_stops_the_read_and_a_missing_file_is_missing)
{
	test_util::temp_dir dir("beneath_stream_stop");
	dir.write("big.txt", std::string(300u * 1024u, 'x'));
	std::size_t chunks = 0;
	const auto stopped = platform::beneath::read_beneath_stream(request_for(dir.path(), "big.txt"),
		[&](std::span<const std::byte>) { return ++chunks < 2u; });
	BOOST_TEST((stopped.status == platform::beneath::read_status::ok));
	BOOST_TEST(chunks == 2u);

	const auto missing = platform::beneath::read_beneath_stream(request_for(dir.path(), "none.txt"),
		[](std::span<const std::byte>) { return true; });
	BOOST_TEST((missing.status == platform::beneath::read_status::missing));
}

BOOST_AUTO_TEST_CASE(a_file_over_the_limit_is_too_large)
{
	test_util::temp_dir dir("beneath_stream_limit");
	dir.write("big.txt", std::string(1000, 'x'));
	auto request = request_for(dir.path(), "big.txt");
	request.max_bytes = 100;
	bool called = false;
	const auto result = platform::beneath::read_beneath_stream(request, [&](std::span<const std::byte>) { called = true; return true; });
	BOOST_TEST((result.status == platform::beneath::read_status::too_large));
	BOOST_TEST(!called);
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(a_link_and_a_fifo_are_not_read)
{
	test_util::temp_dir dir("beneath_stream_kinds");
	dir.write("real.txt", "real");
	std::error_code ec;
	fs::create_symlink(dir.path() / "real.txt", dir.path() / "link.txt", ec);
	BOOST_REQUIRE(!ec);
	BOOST_REQUIRE(::mkfifo((dir.path() / "pipe").c_str(), 0600) == 0);

	bool called = false;
	const auto sink = [&](std::span<const std::byte>) { called = true; return true; };
	BOOST_TEST((platform::beneath::read_beneath_stream(request_for(dir.path(), "link.txt"), sink).status
		== platform::beneath::read_status::not_regular));
	BOOST_TEST((platform::beneath::read_beneath_stream(request_for(dir.path(), "pipe"), sink).status
		== platform::beneath::read_status::not_regular));
	BOOST_TEST(!called);
}
#endif

// #775: a file written from pieces is flushed and moved into place whole; a source that fails leaves no target and no temp file.
BOOST_AUTO_TEST_CASE(a_write_from_pieces_lands_whole_and_a_failed_source_leaves_nothing)
{
	test_util::temp_dir dir("beneath_stream_write");
	const auto write_with = [&](const platform::beneath::chunk_source& source, const char* leaf) {
		platform::beneath::pinned_root root{ dir.path() };
		platform::beneath::write_request request;
		request.folder = "sub";
		request.leaf = leaf;
		request.tmp_leaf = std::string(".") + leaf + ".tmp";
		request.stream = &source;
		request.replace = false;
		request.tmp_path = dir.path() / request.folder / request.tmp_leaf;
		return platform::beneath::write_beneath(root, request);
	};

	const std::string first(100u * 1024u, 'a');
	const std::string second(70u * 1024u, 'b');
	const platform::beneath::chunk_source whole = [&](const platform::beneath::chunk_sink& sink) {
		return sink(std::as_bytes(std::span<const char>(first.data(), first.size())))
			&& sink(std::as_bytes(std::span<const char>(second.data(), second.size())));
	};
	BOOST_TEST(write_with(whole, "whole.bin").empty());
	std::ifstream in(dir.path() / "sub" / "whole.bin", std::ios::binary);
	const std::string read{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	BOOST_TEST((read == first + second));

	const platform::beneath::chunk_source broken = [&](const platform::beneath::chunk_sink& sink) {
		return sink(std::as_bytes(std::span<const char>(first.data(), first.size()))) && false;
	};
	BOOST_TEST(!write_with(broken, "broken.bin").empty());
	BOOST_TEST(!fs::exists(dir.path() / "sub" / "broken.bin"));
	BOOST_TEST(!fs::exists(dir.path() / "sub" / ".broken.bin.tmp"));
}

BOOST_AUTO_TEST_SUITE_END()
