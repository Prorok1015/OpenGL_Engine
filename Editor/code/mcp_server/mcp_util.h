#pragma once
#include <boost/json/string.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Small string helpers of the MCP module. Internal: not part of the include/ surface.
namespace mcp::util
{
	// A JSON string from any text (a std::string converts to boost::json::string without a detour).
	inline boost::json::string js(std::string_view text)
	{
		return boost::json::string(text.data(), text.size());
	}

	char lower_ascii(char c);
	std::string lower(std::string_view text);
	bool iequals(std::string_view a, std::string_view b);
	bool starts_with_nocase(std::string_view text, std::string_view prefix);
	std::string_view trim(std::string_view text);

	// Compares without stopping at the first difference, so the time does not tell how much of a guess was right.
	bool secure_equal(std::string_view given, std::string_view expected);

	// `count` random bytes as lower-case hex, from the system's random device.
	std::string random_hex(std::size_t count);

	// "host", "host:port", "[::1]" or "[::1]:port".
	struct authority
	{
		bool valid = false;
		// Lower-case; an IPv6 literal keeps its brackets.
		std::string host;
		std::optional<unsigned> port;
	};
	authority split_authority(std::string_view text);
	bool is_loopback_host(std::string_view host);

	// `origin` is an Origin header value: a loopback page (any port) or one of `extra`.
	bool origin_allowed(std::string_view origin, const std::vector<std::string>& extra);

	// "Authorization: Bearer <token>" with exactly `token`; false when `token` is empty.
	bool bearer_matches(const std::optional<std::string>& header, std::string_view token);

	// Does an Accept header (absent: anything) allow `type`, e.g. "text/event-stream"?
	bool accepts(const std::optional<std::string>& accept, std::string_view type);
}
