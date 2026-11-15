#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <snk/common/snk_api.h>

// Text someone else chose (a file name, a path, a value read from a file) shown as part of a line a person reads (#991).
namespace text_escape
{
	// What could break, forge or hide a part of a line: controls and format characters (Cc, Cf), every space but U+0020
	// and the line and paragraph separators (Zs, Zl, Zp), Default_Ignorable_Code_Point (zero-width, fill characters,
	// variation selectors, tags...), noncharacters and U+2800 (braille blank).
	SNK_API bool is_hidden_code_point(char32_t c);

	// `text` written out so that nothing in it is hidden: a backslash becomes "\\"; a line feed, a carriage return and a
	// tab become "\n", "\r", "\t"; any other hidden code point becomes "\u{202E}" (uppercase hex, at least 4 digits);
	// each byte that starts no valid UTF-8 sequence becomes "\x{FF}"; everything else is copied.
	// Contract, whatever the input: the result is one line of at most `max_bytes` bytes and never contains a byte below
	// 0x20, DEL, a C1 control, a hidden code point or invalid UTF-8. When cut, the result ends with "..." or is empty
	// (`max_bytes` < 3); a cut is never inside a code point or an escape.
	SNK_API std::string escaped_text(std::string_view text, std::size_t max_bytes);

	// A message of our own as the edge shows it: the input may consist ONLY of our own fixed text plus pieces that already
	// went through escaped_text; raw foreign text is not allowed here (pass it through escaped_text where the message is
	// built). The rules and the contract of escaped_text, except that a backslash is copied as it is, so an escape
	// already in the message is shown once; a raw control, hidden code point or invalid byte is still escaped.
	SNK_API std::string escaped_message(std::string_view text, std::size_t max_bytes);
}
