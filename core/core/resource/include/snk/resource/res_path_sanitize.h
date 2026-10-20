#pragma once
#include <cctype>
#include <string>
#include <string_view>

namespace res
{
	// Turns an untrusted file name (texture path or embedded file name from a model file) into a safe
	// relative path for building a tag: drops empty, "." and ".." parts, root and drive parts, and never
	// returns an empty string. Backslashes are separators on every platform.
	inline std::string sanitize_relative_path(const std::string_view raw)
	{
		std::string out;
		out.reserve(raw.size());

		bool first = true;
		std::size_t pos = 0;
		while (pos <= raw.size()) {
			std::size_t end = raw.find_first_of("/\\", pos);
			if (end == std::string_view::npos) {
				end = raw.size();
			}
			std::string part{ raw.substr(pos, end - pos) };
			pos = end + 1;

			if (first && part.size() >= 2 && std::isalpha(static_cast<unsigned char>(part[0])) && part[1] == ':') {
				part.erase(0, 2);
			}
			first = false;

			if (part.empty() || part == "." || part == "..") {
				continue;
			}
			// ':' would let a part spell a protocol ("x://") or a drive; NUL cannot be part of a file name.
			for (char& c : part) {
				if (c == ':' || c == '\0') {
					c = '_';
				}
			}

			if (!out.empty()) {
				out += '/';
			}
			out += part;
		}

		if (out.empty()) {
			out = "unnamed";
		}
		return out;
	}
}
