#pragma once
#include <algorithm>
#include <initializer_list>
#include <string_view>

// Base only (include_base/, not installed in the SDK). The names a walk below a root refuses on Windows (#1294):
// NtCreateFile makes a name ending in a dot or a space, or a reserved device name, but Win32 and Explorer cannot open
// or remove what it made; a ':' names a stream of another file. Pure, so it is tested on every platform.
namespace platform::beneath {

	inline bool windows_refuses_name(std::u16string_view name)
	{
		if (name.empty()) {
			return false;
		}
		if (name.back() == u'.' || name.back() == u' ' || name.find(u':') != std::u16string_view::npos) {
			return true;
		}
		// The device names hold with any extension, and before it trailing spaces are ignored ("NUL .txt").
		std::u16string_view base = name.substr(0, name.find(u'.'));
		while (!base.empty() && base.back() == u' ') {
			base.remove_suffix(1);
		}
		const auto upper = [](char16_t c) { return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - u'a' + u'A') : c; };
		const auto starts_as = [&](std::u16string_view word) {
			return base.size() >= word.size()
				&& std::equal(word.begin(), word.end(), base.begin(), [&](char16_t a, char16_t b) { return a == upper(b); });
		};
		for (const std::u16string_view device : { u"CON", u"PRN", u"AUX", u"NUL", u"CONIN$", u"CONOUT$" }) {
			if (base.size() == device.size() && starts_as(device)) {
				return true;
			}
		}
		if (base.size() == 4 && (starts_as(u"COM") || starts_as(u"LPT"))) {
			// COM0..COM9 and the superscript digits Windows takes for them too.
			const char16_t digit = base[3];
			return (digit >= u'0' && digit <= u'9') || digit == u'\u00b9' || digit == u'\u00b2' || digit == u'\u00b3';
		}
		return false;
	}

}
