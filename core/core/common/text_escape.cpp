#include <snk/common/text_escape.h>
#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>

namespace {

	// Length of the UTF-8 sequence at the start of `text` and its code point; 0 when it is not a valid one (overlong,
	// surrogate, above U+10FFFF, truncated, stray continuation byte).
	std::size_t decode_utf8(std::string_view text, char32_t& code)
	{
		const auto at = [&text](std::size_t i) { return static_cast<unsigned char>(text[i]); };
		const unsigned char lead = at(0);
		if (lead < 0x80) {
			code = lead;
			return 1;
		}
		std::size_t length = 0;
		char32_t minimum = 0;
		if ((lead & 0xE0) == 0xC0) {
			length = 2;
			code = lead & 0x1Fu;
			minimum = 0x80;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			code = lead & 0x0Fu;
			minimum = 0x800;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			code = lead & 0x07u;
			minimum = 0x10000;
		} else {
			return 0;
		}
		if (text.size() < length) return 0;
		for (std::size_t i = 1; i < length; ++i) {
			if ((at(i) & 0xC0) != 0x80) return 0;
			code = (code << 6) | (at(i) & 0x3Fu);
		}
		if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
		return length;
	}

	struct code_range
	{
		char32_t first;
		char32_t last;
	};

	// Sorted and coalesced, generated with Python's unicodedata (Unicode 15.1.0): categories Cc, Cf, Zs, Zl, Zp but
	// U+0020; Default_Ignorable_Code_Point; noncharacters (U+FDD0..FDEF, U+xFFFE/xFFFF of every plane); U+2800.
	constexpr code_range k_hidden[] = {
		{ 0x0000, 0x001F }, { 0x007F, 0x00A0 }, { 0x00AD, 0x00AD }, { 0x034F, 0x034F }, { 0x0600, 0x0605 },
		{ 0x061C, 0x061C }, { 0x06DD, 0x06DD }, { 0x070F, 0x070F }, { 0x0890, 0x0891 }, { 0x08E2, 0x08E2 },
		{ 0x115F, 0x1160 }, { 0x1680, 0x1680 }, { 0x17B4, 0x17B5 }, { 0x180B, 0x180F }, { 0x2000, 0x200F },
		{ 0x2028, 0x202F }, { 0x205F, 0x206F }, { 0x2800, 0x2800 }, { 0x3000, 0x3000 }, { 0x3164, 0x3164 },
		{ 0xFDD0, 0xFDEF }, { 0xFE00, 0xFE0F }, { 0xFEFF, 0xFEFF }, { 0xFFA0, 0xFFA0 }, { 0xFFF0, 0xFFFB },
		{ 0xFFFE, 0xFFFF }, { 0x110BD, 0x110BD }, { 0x110CD, 0x110CD }, { 0x13430, 0x1343F }, { 0x1BCA0, 0x1BCA3 },
		{ 0x1D173, 0x1D17A }, { 0x1FFFE, 0x1FFFF }, { 0x2FFFE, 0x2FFFF }, { 0x3FFFE, 0x3FFFF }, { 0x4FFFE, 0x4FFFF },
		{ 0x5FFFE, 0x5FFFF }, { 0x6FFFE, 0x6FFFF }, { 0x7FFFE, 0x7FFFF }, { 0x8FFFE, 0x8FFFF }, { 0x9FFFE, 0x9FFFF },
		{ 0xAFFFE, 0xAFFFF }, { 0xBFFFE, 0xBFFFF }, { 0xCFFFE, 0xCFFFF }, { 0xDFFFE, 0xE0FFF }, { 0xEFFFE, 0xEFFFF },
		{ 0xFFFFE, 0xFFFFF }, { 0x10FFFE, 0x10FFFF },
	};

	// Escapes `text` as escaped_text does; `double_backslash` false keeps a backslash as it is (escaped_message).
	std::string escaped(std::string_view text, std::size_t max_bytes, bool double_backslash)
	{
		std::string out;
		out.reserve(std::min(text.size(), max_bytes));
		// The longest prefix of whole pieces that still leaves room for "...".
		std::size_t room_for_dots = 0;
		bool cut = false;
		std::size_t i = 0;
		while (i < text.size()) {
			char32_t code = 0;
			std::size_t length = decode_utf8(text.substr(i), code);
			std::string piece;
			if (length == 0) {
				length = 1;
				piece = std::format("\\x{{{:02X}}}", static_cast<unsigned>(static_cast<unsigned char>(text[i])));
			} else if (code == U'\\') {
				piece = double_backslash ? "\\\\" : "\\";
			} else if (code == U'\n') {
				piece = "\\n";
			} else if (code == U'\r') {
				piece = "\\r";
			} else if (code == U'\t') {
				piece = "\\t";
			} else if (text_escape::is_hidden_code_point(code)) {
				piece = std::format("\\u{{{:04X}}}", static_cast<std::uint32_t>(code));
			} else {
				piece = text.substr(i, length);
			}
			if (out.size() + piece.size() > max_bytes) {
				cut = true;
				break;
			}
			out += piece;
			i += length;
			if (out.size() + 3 <= max_bytes) room_for_dots = out.size();
		}
		if (!cut) return out;
		// A cut result never looks whole: "..." at the end, or nothing when even that does not fit.
		if (max_bytes < 3) return {};
		out.resize(room_for_dots);
		out += "...";
		return out;
	}

}

bool text_escape::is_hidden_code_point(char32_t c)
{
	const auto after = std::upper_bound(std::begin(k_hidden), std::end(k_hidden), c,
		[](char32_t value, const code_range& range) { return value < range.first; });
	return after != std::begin(k_hidden) && c <= std::prev(after)->last;
}

std::string text_escape::escaped_text(std::string_view text, std::size_t max_bytes)
{
	return escaped(text, max_bytes, true);
}

std::string text_escape::escaped_message(std::string_view text, std::size_t max_bytes)
{
	return escaped(text, max_bytes, false);
}
