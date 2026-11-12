#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/platform/plt_beneath.h>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

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

BOOST_AUTO_TEST_SUITE_END()
