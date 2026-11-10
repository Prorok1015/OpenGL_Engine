#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_tag.h>

namespace res
{
	// The tag a file a model refers to names (a texture, a .bin, a .mtl; #631). Model files write these relative to the
	// model ("textures/a.png"), so a reference without a protocol is read in the model's folder and protocol; one with a
	// protocol must name the model's own. "." and ".." are folded; a reference that climbs above the protocol's root, is
	// absolute ("/x", "c:/x") or holds a control character names nothing, and nothing is read for it.
	inline std::optional<tag> model_reference_tag(const tag& model, const std::string_view reference)
	{
		if (model.protocol().empty() || reference.empty() || has_control_chars(reference)) {
			return std::nullopt;
		}
		std::string normalized(reference);
		std::ranges::replace(normalized, '\\', '/');

		std::string joined;
		if (const std::string_view protocol = tag::protocol_of(normalized); !protocol.empty()) {
			if (protocol != model.protocol()) {
				return std::nullopt;
			}
			joined = normalized.substr(protocol.size() + 3);
		} else {
			const bool drive = normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) && normalized[1] == ':';
			if (normalized.front() == '/' || drive || normalized.find("://") != std::string::npos) {
				return std::nullopt;
			}
			joined = std::string(model.path()) + normalized;
		}

		std::vector<std::string_view> segments;
		const std::string_view text = joined;
		std::size_t begin = 0;
		while (begin <= text.size()) {
			const std::size_t end = std::min(text.find('/', begin), text.size());
			const std::string_view segment = text.substr(begin, end - begin);
			if (segment == "..") {
				if (segments.empty()) {
					return std::nullopt;
				}
				segments.pop_back();
			} else if (!segment.empty() && segment != ".") {
				segments.push_back(segment);
			}
			begin = end + 1;
		}
		if (segments.empty()) {
			return std::nullopt;
		}

		std::string relative;
		for (const std::string_view segment : segments) {
			relative += relative.empty() ? "" : "/";
			relative += segment;
		}
		return tag(model.protocol(), relative);
	}
}
