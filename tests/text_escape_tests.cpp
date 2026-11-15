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

	using text_escape::escaped_text;
	using text_escape::is_hidden_code_point;

	struct code_range
	{
		char32_t first;
		char32_t last;
	};

	// The hidden classes as the contract lists them, kept apart from the implementation on purpose.
	const std::vector<code_range> k_hidden_ranges{
		{ 0x0000, 0x001F }, { 0x007F, 0x00A0 }, { 0x00AD, 0x00AD }, { 0x061C, 0x061C }, { 0x115F, 0x1160 }, { 0x180E, 0x180E },
		{ 0x2000, 0x200F }, { 0x2028, 0x202F }, { 0x205F, 0x206F }, { 0x3000, 0x3000 }, { 0x3164, 0x3164 }, { 0xFE00, 0xFE0F },
		{ 0xFEFF, 0xFEFF }, { 0xFFA0, 0xFFA0 }, { 0xFFF9, 0xFFFB }, { 0xE0000, 0xE007F },
	};

	bool in_hidden_ranges(char32_t c)
	{
		for (const code_range& range : k_hidden_ranges) {
			if (c >= range.first && c <= range.last) return true;
		}
		return false;
	}

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

BOOST_AUTO_TEST_CASE(EveryHiddenClass_IsShownAsItsCodePoint)
{
	const std::vector<std::pair<std::string, std::string>> cases{
		{ "\x01", "0001" }, { "\x1B", "001B" }, { "\x1F", "001F" }, { "\x7F", "007F" }, { "\xC2\x85", "0085" },
		{ "\xC2\xAD", "00AD" }, { "\xD8\x9C", "061C" }, { "\xE1\xA0\x8E", "180E" }, { "\xEF\xBB\xBF", "FEFF" },
		{ "\xE2\x80\x8B", "200B" }, { "\xE2\x80\xAE", "202E" }, { "\xE2\x81\xA6", "2066" }, { "\xEF\xBF\xB9", "FFF9" },
		{ "\xF3\xA0\x80\x81", "E0001" }, { "\xF3\xA0\x81\x81", "E0041" }, { "\xE1\x85\x9F", "115F" }, { "\xE3\x85\xA4", "3164" },
		{ "\xEF\xBE\xA0", "FFA0" }, { "\xEF\xB8\x8F", "FE0F" }, { "\xC2\xA0", "00A0" }, { "\xE2\x80\x83", "2003" },
		{ "\xE2\x80\xAF", "202F" }, { "\xE2\x81\x9F", "205F" }, { "\xE3\x80\x80", "3000" },
	};
	for (const auto& [text, code] : cases) {
		BOOST_TEST_CONTEXT(code) {
			BOOST_TEST(escaped_text("a" + text + "b", 100) == "a\\u{" + code + "}b");
		}
	}
}

BOOST_AUTO_TEST_CASE(IsHiddenCodePoint_IsExactlyTheListedClasses)
{
	std::optional<char32_t> wrong;
	for (char32_t c = 0; c <= 0x10FFFF && !wrong; ++c) {
		if (is_hidden_code_point(c) != in_hidden_ranges(c)) wrong = c;
	}
	BOOST_TEST(!wrong.has_value(), "first code point classified otherwise: " << (wrong ? static_cast<std::uint32_t>(*wrong) : 0u));
	BOOST_TEST(!is_hidden_code_point(U' '));
	BOOST_TEST(!is_hidden_code_point(U'a'));
	BOOST_TEST(!is_hidden_code_point(0xA1));
	BOOST_TEST(!is_hidden_code_point(0x2010));
	BOOST_TEST(!is_hidden_code_point(0x1F600));
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
			if (shown == full) continue;
			std::string kept = shown;
			if (max_bytes >= 3) {
				BOOST_TEST(shown.ends_with("..."));
				kept.resize(kept.size() - std::min<std::size_t>(3, kept.size()));
			}
			BOOST_TEST(whole_prefixes.contains(kept), kept);
			// The longest whole prefix that fits is kept.
			const std::size_t room = max_bytes >= 3 ? max_bytes - 3 : max_bytes;
			for (const std::string& prefix : whole_prefixes) {
				if (prefix.size() <= room) BOOST_TEST(prefix.size() <= kept.size(), prefix);
			}
		}
	}
	// The whole of it is longer than every limit above, so each one cuts.
	BOOST_TEST(full.size() > 40);
}

BOOST_AUTO_TEST_CASE(ATinyLimit_KeepsWhatFits_WithoutDots)
{
	BOOST_TEST(escaped_text("abc", 0) == "");
	BOOST_TEST(escaped_text("abc", 1) == "a");
	BOOST_TEST(escaped_text("abc", 2) == "ab");
	BOOST_TEST(escaped_text("\n", 1) == "");
	BOOST_TEST(escaped_text("a\n", 2) == "a");
	BOOST_TEST(escaped_text("\xC3\xA9\xC3\xA9", 3) == "...");
	BOOST_TEST(escaped_text("\xC3\xA9\xC3\xA9", 2) == "\xC3\xA9");
	BOOST_TEST(escaped_text("", 0) == "");
	BOOST_TEST(escaped_text("abc", 3) == "abc");
	BOOST_TEST(escaped_text("abcd", 3) == "...");
}

// Whatever the input, the output keeps the contract: valid UTF-8, no control, no C1, no hidden code point.
BOOST_AUTO_TEST_CASE(AnyInput_GivesALineThatHidesNothing)
{
	std::vector<std::string> inputs;
	for (int code = 0; code < 256; ++code) inputs.emplace_back(1, static_cast<char>(code));
	std::vector<char32_t> points;
	for (char32_t c = 0; c < 0x4000; ++c) points.push_back(c);
	for (char32_t c = 0x4000; c <= 0x10FFFF; c += 16) points.push_back(c);
	for (const code_range& range : k_hidden_ranges) {
		for (const std::uint32_t c : { std::uint32_t{ range.first }, std::uint32_t{ range.last } }) {
			for (const std::uint32_t next_to : { c - 1, c, c + 1 }) {
				if (next_to <= 0x10FFFF) points.push_back(static_cast<char32_t>(next_to));
			}
		}
	}
	points.push_back(0x10FFFF);
	for (const char32_t c : points) {
		if (c >= 0xD800 && c <= 0xDFFF) continue;
		inputs.push_back(encoded(c));
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
				const auto problem = contract_problem(escaped_text(text, max_bytes), max_bytes);
				if (problem && !first_problem) first_problem = hex_of(text) + std::format("(max {}): ", max_bytes) + *problem;
			}
		}
	}
	BOOST_TEST(!first_problem.has_value(), first_problem.value_or(""));
}

BOOST_AUTO_TEST_SUITE_END()
