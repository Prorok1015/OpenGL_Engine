#include <snk/common/text_escape.h>
#include <algorithm>
#include <cstdint>
#include <format>

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

}

bool text_escape::is_hidden_code_point(char32_t c)
{
	if (c < 0x20 || (c >= 0x7F && c <= 0xA0)) return true;
	if (c == 0x00AD || c == 0x061C || c == 0x115F || c == 0x1160 || c == 0x180E) return true;
	if (c >= 0x2000 && c <= 0x200F) return true;
	if (c >= 0x2028 && c <= 0x202F) return true;
	if (c >= 0x205F && c <= 0x206F) return true;
	if (c == 0x3000 || c == 0x3164) return true;
	if (c >= 0xFE00 && c <= 0xFE0F) return true;
	if (c == 0xFEFF || c == 0xFFA0) return true;
	if (c >= 0xFFF9 && c <= 0xFFFB) return true;
	return c >= 0xE0000 && c <= 0xE007F;
}

std::string text_escape::escaped_text(std::string_view text, std::size_t max_bytes)
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
			piece = "\\\\";
		} else if (code == U'\n') {
			piece = "\\n";
		} else if (code == U'\r') {
			piece = "\\r";
		} else if (code == U'\t') {
			piece = "\\t";
		} else if (is_hidden_code_point(code)) {
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
	if (cut && max_bytes >= 3) {
		out.resize(room_for_dots);
		out += "...";
	}
	return out;
}
