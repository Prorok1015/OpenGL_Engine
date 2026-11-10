#pragma once
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <snk/resource/res_tag.h>

namespace edt {

	// A string as res::tag reads it, for every check that must agree with it (the desc files an agent writes, #550, and the
	// descs a decoder process hands back, #647): every backslash is a slash.
	inline std::string tag_text_normalized(std::string_view text)
	{
		std::string copy(text);
		std::replace(copy.begin(), copy.end(), '\\', '/');
		return copy;
	}

	// The protocol res::tag gives a normalized text: never a copy of its rule (#631).
	inline std::string_view tag_protocol(std::string_view normalized)
	{
		return res::tag::protocol_of(normalized);
	}

	// The text before the first ':' or '/' of a normalized text: what a reader could take for a protocol. Not one since #631
	// ("user/x.png" has none), but the checks still refuse a string that begins like a mounted protocol, so no other reader
	// of the file takes it for one.
	inline std::string_view protocol_like_prefix(std::string_view normalized)
	{
		const std::size_t end = normalized.find_first_of(":/");
		return end == std::string_view::npos ? std::string_view{} : normalized.substr(0, end);
	}

	// A place a tag can name that is not the project's res/: every protocol the editor mounts.
	inline bool is_other_tag_protocol(std::string_view protocol)
	{
		constexpr std::string_view mounted[] = { "memory", "engine", "cache", "code", "user", "edt" };
		return std::find(std::begin(mounted), std::end(mounted), protocol) != std::end(mounted);
	}

} // namespace edt
