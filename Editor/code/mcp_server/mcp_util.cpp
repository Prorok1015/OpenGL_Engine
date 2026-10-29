#include "mcp_util.h"
#include <random>

char mcp::util::lower_ascii(char c)
{
	return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string mcp::util::lower(std::string_view text)
{
	std::string out(text);
	for (char& c : out) c = lower_ascii(c);
	return out;
}

bool mcp::util::iequals(std::string_view a, std::string_view b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i) {
		if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
	}
	return true;
}

bool mcp::util::starts_with_nocase(std::string_view text, std::string_view prefix)
{
	return text.size() >= prefix.size() && iequals(text.substr(0, prefix.size()), prefix);
}

std::string_view mcp::util::trim(std::string_view text)
{
	const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	while (!text.empty() && is_space(text.front())) text.remove_prefix(1);
	while (!text.empty() && is_space(text.back())) text.remove_suffix(1);
	return text;
}

bool mcp::util::secure_equal(std::string_view given, std::string_view expected)
{
	std::size_t difference = given.size() ^ expected.size();
	for (std::size_t i = 0; i < expected.size(); ++i) {
		const char g = given.empty() ? '\0' : given[i % given.size()];
		difference |= static_cast<unsigned char>(g) ^ static_cast<unsigned char>(expected[i]);
	}
	return difference == 0;
}

std::string mcp::util::random_hex(std::size_t count)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::random_device device;
	std::string out;
	out.reserve(count * 2);
	std::uint32_t pool = 0;
	int left = 0;
	for (std::size_t i = 0; i < count; ++i) {
		if (left == 0) {
			pool = device();
			left = 4;
		}
		const unsigned byte = pool & 0xFFu;
		pool >>= 8;
		--left;
		out.push_back(digits[byte >> 4]);
		out.push_back(digits[byte & 0xFu]);
	}
	return out;
}

namespace {
	std::optional<unsigned> parse_port(std::string_view text)
	{
		if (text.empty() || text.size() > 5) return std::nullopt;
		unsigned value = 0;
		for (char c : text) {
			if (c < '0' || c > '9') return std::nullopt;
			value = value * 10 + static_cast<unsigned>(c - '0');
		}
		if (value > 65535) return std::nullopt;
		return value;
	}
}

mcp::util::authority mcp::util::split_authority(std::string_view text)
{
	authority out;
	text = trim(text);
	if (text.empty()) return out;

	std::string_view host_part = text;
	std::string_view port_part;
	bool has_port = false;
	if (text.front() == '[') {
		const std::size_t close = text.find(']');
		if (close == std::string_view::npos) return out;
		host_part = text.substr(0, close + 1);
		const std::string_view rest = text.substr(close + 1);
		if (!rest.empty()) {
			if (rest.front() != ':') return out;
			port_part = rest.substr(1);
			has_port = true;
		}
	} else {
		const std::size_t colon = text.rfind(':');
		if (colon != std::string_view::npos) {
			host_part = text.substr(0, colon);
			port_part = text.substr(colon + 1);
			has_port = true;
		}
		// A bare IPv6 address (colons without brackets) is not a valid Host value.
		if (host_part.find(':') != std::string_view::npos) return out;
	}
	if (host_part.empty()) return out;
	if (has_port) {
		out.port = parse_port(port_part);
		if (!out.port) return out;
	}
	out.host = lower(host_part);
	out.valid = true;
	return out;
}

bool mcp::util::is_loopback_host(std::string_view host)
{
	return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

bool mcp::util::origin_allowed(std::string_view origin, const std::vector<std::string>& extra)
{
	origin = trim(origin);
	for (const std::string& allowed : extra) {
		if (iequals(origin, allowed)) return true;
	}
	const std::size_t scheme_end = origin.find("://");
	if (scheme_end == std::string_view::npos) return false;
	const std::string scheme = lower(origin.substr(0, scheme_end));
	if (scheme != "http" && scheme != "https") return false;
	const std::string_view rest = origin.substr(scheme_end + 3);
	if (rest.find('/') != std::string_view::npos) return false;
	const authority where = split_authority(rest);
	return where.valid && is_loopback_host(where.host);
}

bool mcp::util::bearer_matches(const std::optional<std::string>& header, std::string_view token)
{
	if (token.empty() || !header) return false;
	const std::string_view value = trim(*header);
	constexpr std::string_view scheme = "bearer";
	if (value.size() <= scheme.size() || !iequals(value.substr(0, scheme.size()), scheme)) return false;
	if (value[scheme.size()] != ' ') return false;
	return secure_equal(trim(value.substr(scheme.size() + 1)), token);
}

bool mcp::util::accepts(const std::optional<std::string>& accept, std::string_view type)
{
	if (!accept) return true;
	const std::size_t slash = type.find('/');
	const std::string wanted = lower(type);
	const std::string group = slash == std::string_view::npos ? std::string() : lower(type.substr(0, slash)) + "/*";

	std::string_view rest = *accept;
	while (!rest.empty()) {
		const std::size_t comma = rest.find(',');
		std::string_view item = trim(rest.substr(0, comma));
		rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
		const std::size_t params = item.find(';');
		if (params != std::string_view::npos) item = trim(item.substr(0, params));
		const std::string name = lower(item);
		if (name == wanted || name == "*/*" || (!group.empty() && name == group)) return true;
	}
	return false;
}
