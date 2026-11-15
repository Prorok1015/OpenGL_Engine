#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/common/hash/sha256.h>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// cmn::sha256_hex / sha256_file_hex (#921) against the FIPS 180-4 examples and the padding edges of one block.

namespace {

std::string sha(std::string_view text)
{
	return cmn::sha256_hex(std::as_bytes(std::span{ text.data(), text.size() }));
}

// 0, 1, ..., 255 repeated: 76800 bytes, more than one read chunk of sha256_file_hex.
std::string counting_bytes()
{
	std::string text;
	text.reserve(256 * 300);
	for (int round = 0; round < 300; ++round) {
		for (int b = 0; b < 256; ++b) {
			text.push_back(static_cast<char>(b));
		}
	}
	return text;
}

}

BOOST_AUTO_TEST_SUITE(common_sha256_tests)

BOOST_AUTO_TEST_CASE(the_fips_examples_hash_to_their_published_digests)
{
	BOOST_TEST(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	BOOST_TEST(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	BOOST_TEST(sha("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
		== "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
	BOOST_TEST(sha(std::string(1'000'000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

BOOST_AUTO_TEST_CASE(lengths_at_the_padding_edges_hash_right)
{
	// 55 bytes still take the length in their block, 56 need a second one, 64 fill one exactly.
	BOOST_TEST(sha(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
	BOOST_TEST(sha(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
	BOOST_TEST(sha(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

BOOST_AUTO_TEST_CASE(a_file_hashes_like_its_bytes_and_a_missing_one_has_no_hash)
{
	test_util::temp_dir dir("common_sha256");
	const std::string bytes = counting_bytes();
	const auto file = dir.write("lib.bin", bytes);
	const auto empty = dir.write("empty.bin", "");

	const std::optional<std::string> digest = cmn::sha256_file_hex(file);
	BOOST_REQUIRE(digest.has_value());
	BOOST_TEST(*digest == "f8b0585eb91f58c007a5634362c9f90d8543822c113f702523bc7b73408a9392");
	BOOST_TEST(*digest == sha(bytes));
	const std::optional<std::string> empty_digest = cmn::sha256_file_hex(empty);
	BOOST_TEST((empty_digest && *empty_digest == sha("")));
	BOOST_TEST(!cmn::sha256_file_hex(dir.path() / "missing.bin").has_value());
	BOOST_TEST(!cmn::sha256_file_hex(dir.path()).has_value()); // a folder is not a file
}

BOOST_AUTO_TEST_SUITE_END()
