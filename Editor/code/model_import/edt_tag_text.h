#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

namespace edt {

	// How a string is read as a tag by res::tag, in one place for every check that must agree with it (the desc files an agent
	// writes, #550, and the descs a decoder process hands back, #647): every backslash is a slash, and the text up to the first
	// ':' or '/' is the protocol, so "user/x.png", "user:/x.png" and "user\x.png" are all user://x.png.
	inline std::string tag_text_normalized(std::string_view text)
	{
		std::string copy(text);
		std::replace(copy.begin(), copy.end(), '\\', '/');
		return copy;
	}

	// The protocol of an already normalized text; empty when it has no ':' or '/'.
	inline std::string_view tag_leading_protocol(const std::string& normalized)
	{
		const std::size_t end = normalized.find_first_of(":/");
		return end == std::string::npos ? std::string_view{} : std::string_view(normalized).substr(0, end);
	}

	// A place a tag can name that is not the project's res/: every protocol the editor mounts.
	inline bool is_other_tag_protocol(std::string_view protocol)
	{
		constexpr std::string_view mounted[] = { "memory", "engine", "cache", "code", "user", "edt" };
		return std::find(std::begin(mounted), std::end(mounted), protocol) != std::end(mounted);
	}

} // namespace edt
