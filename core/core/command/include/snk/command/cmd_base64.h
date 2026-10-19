#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <snk/common/snk_api.h>

namespace cmd::base64
{
	// Binary data in a JSON result (a PNG of `viewport.capture`) travels as standard base64 (RFC 4648, with padding).
	[[nodiscard]] SNK_API std::string encode(std::span<const std::byte> data);

	// Nullopt when `text` is not valid padded base64.
	[[nodiscard]] SNK_API std::optional<std::vector<std::byte>> decode(std::string_view text);
}
