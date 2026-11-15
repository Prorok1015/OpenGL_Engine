#include <boost/test/unit_test.hpp>
#include <snk/common/text_escape.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// text_escape::escaped_text (#991): the one function that shows untrusted text on a line. Non-ASCII text is spelled in
// escapes: MSVC reads the source in the local code page.

namespace {

	using text_escape::escaped_message;
	using text_escape::escaped_text;
	using text_escape::is_hidden_code_point;

	// UTF-8 bytes of `c`, surrogates included (a caller that wants a malformed sequence asks for one).
	std::string encoded(char32_t c)
	{
		std::string out;
		if (c < 0x80) {
			out += static_cast<char>(c);
		} else if (c < 0x800) {
			out += static_cast<char>(0xC0 | (c >> 6));
			out += static_cast<char>(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out += static_cast<char>(0xE0 | (c >> 12));
			out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (c & 0x3F));
		} else {
			out += static_cast<char>(0xF0 | (c >> 18));
			out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
			out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
			out += static_cast<char>(0x80 | (c & 0x3F));
		}
		return out;
	}

	// Strict UTF-8 decoding of all of `text`; nullopt when any of it is not valid UTF-8.
	std::optional<std::vector<char32_t>> decoded(std::string_view text)
	{
		std::vector<char32_t> points;
		std::size_t i = 0;
		while (i < text.size()) {
			const auto byte_at = [&text](std::size_t at) { return static_cast<std::uint32_t>(static_cast<unsigned char>(text[at])); };
			const std::uint32_t lead = byte_at(i);
			std::size_t length = 0;
			std::uint32_t value = 0;
			std::uint32_t least = 0;
			if (lead < 0x80) {
				length = 1;
				value = lead;
			} else if (lead >= 0xC0 && lead < 0xE0) {
				length = 2;
				value = lead & 0x1F;
				least = 0x80;
			} else if (lead >= 0xE0 && lead < 0xF0) {
				length = 3;
				value = lead & 0x0F;
				least = 0x800;
			} else if (lead >= 0xF0 && lead < 0xF8) {
				length = 4;
				value = lead & 0x07;
				least = 0x10000;
			} else {
				return std::nullopt;
			}
			if (i + length > text.size()) return std::nullopt;
			for (std::size_t k = 1; k < length; ++k) {
				if ((byte_at(i + k) & 0xC0) != 0x80) return std::nullopt;
				value = (value << 6) | (byte_at(i + k) & 0x3F);
			}
			if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) return std::nullopt;
			points.push_back(static_cast<char32_t>(value));
			i += length;
		}
		return points;
	}

	// What breaks the contract in `shown`, the result for `max_bytes`; nullopt when nothing does.
	std::optional<std::string> contract_problem(std::string_view shown, std::size_t max_bytes)
	{
		if (shown.size() > max_bytes) return std::format("{} bytes, more than {}", shown.size(), max_bytes);
		const auto points = decoded(shown);
		if (!points) return std::string("not valid UTF-8");
		for (const char32_t c : *points) {
			if (c < 0x20) return std::format("a byte below 0x20: {:02X}", static_cast<std::uint32_t>(c));
			if (c == 0x7F) return std::string("DEL");
			if (c >= 0x80 && c <= 0x9F) return std::format("a C1 control: {:04X}", static_cast<std::uint32_t>(c));
			if (is_hidden_code_point(c)) return std::format("a hidden code point: {:04X}", static_cast<std::uint32_t>(c));
		}
		return std::nullopt;
	}

	std::string hex_of(std::string_view bytes)
	{
		std::string out;
		for (const char c : bytes) out += std::format("{:02X} ", static_cast<unsigned>(static_cast<unsigned char>(c)));
		return out;
	}

	// The input mixes every kind of piece: plain, doubled, short escapes, \u{...}, \x{..}, 2- and 4-byte characters.
	const std::string k_mixed = "a\\b\n\xD0\x9F\xE2\x80\xAE\xFF" "c\xF0\x9F\x98\x80" "d\xF3\xA0\x80\x81" "end\t";

}

BOOST_AUTO_TEST_SUITE(TextEscape)

BOOST_AUTO_TEST_CASE(ABackslash_IsDoubled)
{
	BOOST_TEST(escaped_text("\\", 100) == "\\\\");
	BOOST_TEST(escaped_text("C:\\models\\n.glb", 100) == "C:\\\\models\\\\n.glb");
}

BOOST_AUTO_TEST_CASE(ALineBreakACarriageReturnAndATab_AreTwoVisibleCharacters)
{
	BOOST_TEST(escaped_text("a\nb", 100) == "a\\nb");
	BOOST_TEST(escaped_text("a\rb", 100) == "a\\rb");
	BOOST_TEST(escaped_text("a\tb", 100) == "a\\tb");
}

BOOST_AUTO_TEST_CASE(EachCodePointOfTheHandTable_IsShownAsWritten)
{
	struct expected_shown
	{
		char32_t code;
		std::string_view shown;
		bool hidden;
	};
	// Written by hand from the contract (controls, format characters, spaces but U+0020, separators,
	// Default_Ignorable_Code_Point, noncharacters, U+2800), not from the implementation's table. U+FFFC (So, the object
	// replacement glyph) draws something and is copied; so are the neighbours of the hidden ranges.
	const std::vector<expected_shown> table{
		{ 0x0000, "\\u{0000}", true }, { 0x001F, "\\u{001F}", true }, { 0x007F, "\\u{007F}", true }, { 0x0085, "\\u{0085}", true },
		{ 0x00A0, "\\u{00A0}", true }, { 0x00AD, "\\u{00AD}", true }, { 0x034F, "\\u{034F}", true }, { 0x061C, "\\u{061C}", true },
		{ 0x115F, "\\u{115F}", true }, { 0x1160, "\\u{1160}", true }, { 0x1680, "\\u{1680}", true }, { 0x17B4, "\\u{17B4}", true },
		{ 0x17B5, "\\u{17B5}", true }, { 0x180B, "\\u{180B}", true }, { 0x180C, "\\u{180C}", true }, { 0x180D, "\\u{180D}", true },
		{ 0x180E, "\\u{180E}", true }, { 0x180F, "\\u{180F}", true }, { 0x2000, "\\u{2000}", true }, { 0x200A, "\\u{200A}", true },
		{ 0x200B, "\\u{200B}", true }, { 0x200F, "\\u{200F}", true }, { 0x2028, "\\u{2028}", true }, { 0x2029, "\\u{2029}", true },
		{ 0x202A, "\\u{202A}", true }, { 0x202E, "\\u{202E}", true }, { 0x202F, "\\u{202F}", true }, { 0x205F, "\\u{205F}", true },
		{ 0x2060, "\\u{2060}", true }, { 0x206F, "\\u{206F}", true }, { 0x2800, "\\u{2800}", true }, { 0x3000, "\\u{3000}", true },
		{ 0x3164, "\\u{3164}", true }, { 0xFDD0, "\\u{FDD0}", true }, { 0xFDEF, "\\u{FDEF}", true }, { 0xFE00, "\\u{FE00}", true },
		{ 0xFE0F, "\\u{FE0F}", true }, { 0xFEFF, "\\u{FEFF}", true }, { 0xFFA0, "\\u{FFA0}", true }, { 0xFFF0, "\\u{FFF0}", true },
		{ 0xFFF8, "\\u{FFF8}", true }, { 0xFFF9, "\\u{FFF9}", true }, { 0xFFFB, "\\u{FFFB}", true }, { 0xFFFE, "\\u{FFFE}", true },
		{ 0xFFFF, "\\u{FFFF}", true }, { 0x13430, "\\u{13430}", true }, { 0x1343F, "\\u{1343F}", true }, { 0x1BCA0, "\\u{1BCA0}", true },
		{ 0x1BCA3, "\\u{1BCA3}", true }, { 0x1D173, "\\u{1D173}", true }, { 0x1D17A, "\\u{1D17A}", true }, { 0x1FFFE, "\\u{1FFFE}", true },
		{ 0xE0000, "\\u{E0000}", true }, { 0xE0001, "\\u{E0001}", true }, { 0xE007F, "\\u{E007F}", true }, { 0xE0080, "\\u{E0080}", true },
		{ 0xE0100, "\\u{E0100}", true }, { 0xE01EF, "\\u{E01EF}", true }, { 0xE0FFF, "\\u{E0FFF}", true }, { 0x10FFFF, "\\u{10FFFF}", true },
		{ 0x0041, "A", false }, { 0x00A1, "\xC2\xA1", false }, { 0x034E, "\xCD\x8E", false }, { 0x0350, "\xCD\x90", false },
		{ 0x17B3, "\xE1\x9E\xB3", false }, { 0x17B6, "\xE1\x9E\xB6", false }, { 0x1681, "\xE1\x9A\x81", false }, { 0x27FF, "\xE2\x9F\xBF", false },
		{ 0x2801, "\xE2\xA0\x81", false }, { 0xFDCF, "\xEF\xB7\x8F", false }, { 0xFDF0, "\xEF\xB7\xB0", false }, { 0xFFFC, "\xEF\xBF\xBC", false },
		{ 0xFFFD, "\xEF\xBF\xBD", false }, { 0x1D172, "\xF0\x9D\x85\xB2", false }, { 0x1D17B, "\xF0\x9D\x85\xBB", false }, { 0xE1000, "\xF3\xA1\x80\x80", false },
		{ 0x0416, "\xD0\x96", false }, { 0x4E2D, "\xE4\xB8\xAD", false }, { 0x1F600, "\xF0\x9F\x98\x80", false },
	};
	for (const expected_shown& entry : table) {
		BOOST_TEST_CONTEXT(std::format("U+{:04X}", static_cast<std::uint32_t>(entry.code))) {
			BOOST_TEST(escaped_text(encoded(entry.code), 64) == entry.shown);
			BOOST_TEST(is_hidden_code_point(entry.code) == entry.hidden);
		}
	}
}

BOOST_AUTO_TEST_CASE(OrdinaryTextOfAnyScript_IsCopied)
{
	const std::string cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
	const std::string cjk = "\xE4\xB8\xAD\xE6\x96\x87";
	const std::string emoji = "\xF0\x9F\x98\x80";
	BOOST_TEST(escaped_text(cyrillic, 100) == cyrillic);
	BOOST_TEST(escaped_text(cjk, 100) == cjk);
	BOOST_TEST(escaped_text(emoji, 100) == emoji);
	BOOST_TEST(escaped_text("res/models/robot.glb", 100) == "res/models/robot.glb");
}

BOOST_AUTO_TEST_CASE(EachByteOfInvalidUtf8_IsShownAsItsHex)
{
	BOOST_TEST(escaped_text("\x80", 100) == "\\x{80}");
	BOOST_TEST(escaped_text("\xC0\xAF", 100) == "\\x{C0}\\x{AF}");
	BOOST_TEST(escaped_text("\xED\xA0\x80", 100) == "\\x{ED}\\x{A0}\\x{80}");
	BOOST_TEST(escaped_text("\xF4\x90\x80\x80", 100) == "\\x{F4}\\x{90}\\x{80}\\x{80}");
	BOOST_TEST(escaped_text("\xE2\x82", 100) == "\\x{E2}\\x{82}");
	// Decoding goes on at the next byte: a valid character after a bad byte is copied.
	BOOST_TEST(escaped_text("\xE2\x82" "a\xC3\xA9", 100) == "\\x{E2}\\x{82}a\xC3\xA9");
}

BOOST_AUTO_TEST_CASE(APathWithALineBreak_StaysOneLine)
{
	const std::string shown = escaped_text("res/a.desc\nThis rollback is safe\r\n\xE2\x80\xA8" "fdp.desc", 200);
	BOOST_TEST(shown.find('\n') == std::string::npos);
	BOOST_TEST(shown.find('\r') == std::string::npos);
	BOOST_TEST(shown == "res/a.desc\\nThis rollback is safe\\r\\n\\u{2028}fdp.desc");
}

BOOST_AUTO_TEST_CASE(ACut_KeepsWholePieces_AndNeverGoesOverTheLimit)
{
	const std::string full = escaped_text(k_mixed, 1000);
	// Every output a cut may keep: the escaping of a prefix of the input that ends between two pieces.
	std::set<std::string> whole_prefixes;
	for (std::size_t k = 0; k <= k_mixed.size(); ++k) {
		const std::string prefix = escaped_text(std::string_view(k_mixed).substr(0, k), 1000);
		if (full.starts_with(prefix)) whole_prefixes.insert(prefix);
	}
	for (std::size_t max_bytes = 0; max_bytes <= 40; ++max_bytes) {
		BOOST_TEST_CONTEXT("max_bytes " << max_bytes) {
			const std::string shown = escaped_text(k_mixed, max_bytes);
			const auto problem = contract_problem(shown, max_bytes);
			BOOST_TEST(!problem.has_value(), problem.value_or(""));
			// A cut result never looks whole: "..." at the end, or nothing when that does not fit.
			if (max_bytes < 3) {
				BOOST_TEST(shown.empty(), shown);
				continue;
			}
			BOOST_REQUIRE(shown.ends_with("..."));
			const std::string kept = shown.substr(0, shown.size() - 3);
			BOOST_TEST(whole_prefixes.contains(kept), kept);
			// The longest whole prefix that fits is kept.
			for (const std::string& prefix : whole_prefixes) {
				if (prefix.size() + 3 <= max_bytes) BOOST_TEST(prefix.size() <= kept.size(), prefix);
			}
		}
	}
	// The whole of it is longer than every limit above, so each one cuts.
	BOOST_TEST(full.size() > 40);
}

// The 9-byte escape of U+E0001: a cut at any byte of it keeps it whole or leaves it out.
BOOST_AUTO_TEST_CASE(ACutAtTheLongestEscape_KeepsItWholeOrLeavesItOut)
{
	const std::string text = "ab\xF3\xA0\x80\x81" "c";
	const std::string full = "ab\\u{E0001}c";
	const std::vector<std::string> whole_prefixes{ "", "a", "ab", "ab\\u{E0001}", full };
	for (std::size_t max_bytes = 0; max_bytes <= full.size(); ++max_bytes) {
		BOOST_TEST_CONTEXT("max_bytes " << max_bytes) {
			const std::string shown = escaped_text(text, max_bytes);
			std::string expected;
			if (max_bytes >= full.size()) {
				expected = full;
			} else if (max_bytes >= 3) {
				for (const std::string& prefix : whole_prefixes) {
					if (prefix.size() + 3 <= max_bytes) expected = prefix + "...";
				}
			}
			BOOST_TEST(shown == expected);
			BOOST_TEST(!contract_problem(shown, max_bytes).has_value());
		}
	}
}

BOOST_AUTO_TEST_CASE(ATinyLimit_ShowsNothingOfACutText)
{
	BOOST_TEST(escaped_text("abc", 0) == "");
	BOOST_TEST(escaped_text("abc", 1) == "");
	BOOST_TEST(escaped_text("abc", 2) == "");
	BOOST_TEST(escaped_text("ab", 2) == "ab");
	BOOST_TEST(escaped_text("a", 1) == "a");
	BOOST_TEST(escaped_text("\n", 1) == "");
	BOOST_TEST(escaped_text("\n", 2) == "\\n");
	BOOST_TEST(escaped_text("a\n", 2) == "");
	BOOST_TEST(escaped_text("\xC3\xA9\xC3\xA9", 3) == "...");
	BOOST_TEST(escaped_text("\xC3\xA9\xC3\xA9", 2) == "");
	BOOST_TEST(escaped_text("\xC3\xA9", 2) == "\xC3\xA9");
	BOOST_TEST(escaped_text("", 0) == "");
	BOOST_TEST(escaped_text("abc", 3) == "abc");
	BOOST_TEST(escaped_text("abcd", 3) == "...");
}

// A message of our own that already holds escapes shows them once; whatever raw slipped in is still escaped.
BOOST_AUTO_TEST_CASE(AMessage_KeepsItsBackslashes_AndEscapesTheRest)
{
	BOOST_TEST(escaped_message("C:\\models\\n.glb", 100) == "C:\\models\\n.glb");
	const std::string piece = escaped_text("a\xE2\x80\xAE" "b\nc\\d", 100);
	BOOST_TEST(piece == "a\\u{202E}b\\nc\\\\d");
	BOOST_TEST(escaped_message("'" + piece + "' is refused", 100) == "'" + piece + "' is refused");
	BOOST_TEST(escaped_message("x\xE2\x80\xAEy\nz\t\xFF", 100) == "x\\u{202E}y\\nz\\t\\x{FF}");
	BOOST_TEST(escaped_message("abcd", 3) == "...");
	BOOST_TEST(escaped_message("abc", 2) == "");
}

// Whatever the input, the output of either function keeps the contract: valid UTF-8, no control, no C1, no hidden code
// point, never longer than its limit (escaped_message may hold a backslash that starts no escape: not checked).
BOOST_AUTO_TEST_CASE(AnyInput_GivesALineThatHidesNothing)
{
	std::vector<std::string> inputs;
	for (int code = 0; code < 256; ++code) inputs.emplace_back(1, static_cast<char>(code));
	std::vector<std::uint32_t> points;
	for (std::uint32_t c = 0; c < 0x4000; ++c) points.push_back(c);
	for (std::uint32_t c = 0x4000; c <= 0x10FFFF; c += 64) points.push_back(c);
	const auto every_one = [&points](std::uint32_t first, std::uint32_t last) {
		for (std::uint32_t c = first; c <= last; ++c) points.push_back(c);
	};
	every_one(0xFDC0, 0xFE1F);
	every_one(0xFFF0, 0xFFFF);
	every_one(0xE0000, 0xE01FF);
	// Every edge of the hidden set, found by walking it (the test keeps no copy of its table), with 64 code points on
	// each side.
	bool was_hidden = is_hidden_code_point(0);
	for (std::uint32_t c = 1; c <= 0x10FFFF; ++c) {
		const bool hidden = is_hidden_code_point(static_cast<char32_t>(c));
		if (hidden == was_hidden) continue;
		was_hidden = hidden;
		every_one(c < 64 ? 0 : c - 64, std::min<std::uint32_t>(c + 64, 0x10FFFF));
	}
	for (const std::uint32_t c : points) {
		if (c >= 0xD800 && c <= 0xDFFF) continue;
		inputs.push_back(encoded(static_cast<char32_t>(c)));
	}
	// Surrogates encoded as if they were code points: never valid UTF-8.
	for (char32_t c = 0xD800; c <= 0xDFFF; ++c) inputs.push_back(encoded(c));
	for (const std::string_view bad : { "\x80", "\xBF", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\x80", "\xE0\x9F\xBF", "\xED\xA0\x80",
			 "\xED\xBF\xBF", "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xF8\x88\x80\x80\x80",
			 "\xFC\x84\x80\x80\x80\x80", "\xFE", "\xFF", "\xE2\x82", "\xF0\x9F\x98", "\xC3", "\xC3" "a", "\xE2\x28\xA1",
			 "\xF0\x9F" "\xE2\x80\xAE", "\xC2\x85\xC2", "\\\xC2" }) {
		inputs.emplace_back(bad);
	}
	std::optional<std::string> first_problem;
	for (const std::string& input : inputs) {
		for (const std::string& text : { input, "a" + input + "b" }) {
			for (const std::size_t max_bytes : { std::size_t{ 64 }, std::size_t{ 7 } }) {
				for (const bool message : { false, true }) {
					const std::string shown = message ? escaped_message(text, max_bytes) : escaped_text(text, max_bytes);
					const auto problem = contract_problem(shown, max_bytes);
					if (problem && !first_problem) {
						first_problem = hex_of(text) + std::format("({}, max {}): ", message ? "message" : "text", max_bytes) + *problem;
					}
				}
			}
		}
	}
	BOOST_TEST(!first_problem.has_value(), first_problem.value_or(""));
}

BOOST_AUTO_TEST_SUITE_END()
