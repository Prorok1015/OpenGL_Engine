#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_file_identity.h"
#include <cstddef>
#include <algorithm>
#include <format>
#include <string>
#include <span>
#include <string_view>

namespace {

namespace fs = std::filesystem;

std::string hex_of(const edt::content_digest& digest)
{
	std::string out;
	for (const std::uint8_t byte : digest) out += std::format("{:02x}", byte);
	return out;
}

edt::content_digest digest_of_text(std::string_view text)
{
	return edt::digest_of(std::as_bytes(std::span(text.data(), text.size())));
}

}

BOOST_AUTO_TEST_SUITE(EdtFileIdentityTests)

BOOST_AUTO_TEST_CASE(Sha256_MatchesTheKnownVectors)
{
	BOOST_CHECK_EQUAL(hex_of(digest_of_text("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
	BOOST_CHECK_EQUAL(hex_of(digest_of_text("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	BOOST_CHECK_EQUAL(hex_of(digest_of_text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
		"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

BOOST_AUTO_TEST_CASE(Sha256_OfPiecesEqualsTheDigestOfTheWhole)
{
	const std::string text(1000, 'x');
	edt::content_hasher hasher;
	for (std::size_t at = 0; at < text.size(); at += 37) {
		const std::size_t part = (std::min<std::size_t>)(37, text.size() - at);
		hasher.update(std::as_bytes(std::span(text.data() + at, part)));
	}
	BOOST_CHECK(hasher.finish() == digest_of_text(text));
}

BOOST_AUTO_TEST_CASE(FileId_IsTheSameForTheSameFile_AndAnotherAfterAReplacement)
{
	test_util::temp_dir dir{ "file_identity" };
	const fs::path file = dir.write("a.bin", "one");
	const edt::file_id first = edt::file_id_of(file);
	BOOST_REQUIRE(first.valid);
	BOOST_CHECK(edt::file_id_of(dir.path() / "." / "a.bin") == first);

	dir.write("b.bin", "two");
	BOOST_CHECK(edt::file_id_of(dir.path() / "b.bin") != first);

	fs::rename(dir.path() / "b.bin", file);
	BOOST_CHECK(edt::file_id_of(file) != first);
	BOOST_CHECK(!edt::file_id_of(dir.path() / "missing.bin").valid);
}

BOOST_AUTO_TEST_SUITE_END()
