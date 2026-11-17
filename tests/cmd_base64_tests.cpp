#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_base64.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Binary results of commands (a PNG) travel as standard padded base64.

namespace {

std::vector<std::byte> bytes_of(std::string_view text)
{
	std::vector<std::byte> out;
	for (const char c : text) {
		out.push_back(static_cast<std::byte>(c));
	}
	return out;
}

std::string text_of(const std::vector<std::byte>& bytes)
{
	std::string out;
	for (const std::byte b : bytes) {
		out.push_back(static_cast<char>(b));
	}
	return out;
}

}

BOOST_AUTO_TEST_SUITE(CmdBase64Tests)

BOOST_AUTO_TEST_CASE(Encode_MatchesTheRfc4648Vectors)
{
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("")), "");
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("f")), "Zg==");
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("fo")), "Zm8=");
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("foo")), "Zm9v");
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("foob")), "Zm9vYg==");
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("fooba")), "Zm9vYmE=");
	BOOST_CHECK_EQUAL(cmd::base64::encode(bytes_of("foobar")), "Zm9vYmFy");
}

BOOST_AUTO_TEST_CASE(Decode_MatchesTheRfc4648Vectors)
{
	const std::pair<const char*, const char*> vectors[] = { { "", "" }, { "Zg==", "f" }, { "Zm8=", "fo" }, { "Zm9v", "foo" },
		{ "Zm9vYg==", "foob" }, { "Zm9vYmE=", "fooba" }, { "Zm9vYmFy", "foobar" } };
	for (const auto& [encoded, plain] : vectors) {
		const auto decoded = cmd::base64::decode(encoded);
		BOOST_REQUIRE_MESSAGE(decoded.has_value(), encoded);
		BOOST_CHECK_EQUAL(text_of(*decoded), plain);
	}
}

BOOST_AUTO_TEST_CASE(RoundTrip_KeepsEveryByteValue)
{
	std::vector<std::byte> all;
	for (int round = 0; round < 3; ++round) {
		for (int value = 0; value < 256; ++value) {
			all.push_back(static_cast<std::byte>(value));
		}
		const auto decoded = cmd::base64::decode(cmd::base64::encode(all));
		BOOST_REQUIRE(decoded.has_value());
		BOOST_CHECK(*decoded == all);
		all.push_back(std::byte{ 7 });
	}
}

BOOST_AUTO_TEST_CASE(Decode_RefusesWhatIsNotPaddedBase64)
{
	for (const char* bad : { "Zg=", "Zg", "Z!==", "Zm9v Zg==", "Zg==Zg==", "====", "Zg=A", "Zm9\n" }) {
		BOOST_CHECK_MESSAGE(!cmd::base64::decode(bad).has_value(), bad);
	}
}

BOOST_AUTO_TEST_SUITE_END()
