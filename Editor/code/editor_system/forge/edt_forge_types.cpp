#include "edt_forge_types.h"
#include <algorithm>
#include <cctype>

namespace {

	constexpr std::string_view k_replacement = "\xEF\xBF\xBD";

	// One UTF-8 sequence at the start of `text` (strict: no overlong forms, no surrogates, at most U+10FFFF).
	// Returns its length and the code point, or length 0 for an invalid start.
	std::size_t decode_utf8(std::string_view text, char32_t& code)
	{
		const auto at = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
		const unsigned char lead = at(0);
		if (lead < 0x80) {
			code = lead;
			return 1;
		}
		std::size_t length = 0;
		char32_t minimum = 0;
		if ((lead & 0xE0) == 0xC0) {
			length = 2;
			code = lead & 0x1F;
			minimum = 0x80;
		} else if ((lead & 0xF0) == 0xE0) {
			length = 3;
			code = lead & 0x0F;
			minimum = 0x800;
		} else if ((lead & 0xF8) == 0xF0) {
			length = 4;
			code = lead & 0x07;
			minimum = 0x10000;
		} else {
			return 0;
		}
		if (text.size() < length) return 0;
		for (std::size_t i = 1; i < length; ++i) {
			if ((at(i) & 0xC0) != 0x80) return 0;
			code = (code << 6) | (at(i) & 0x3F);
		}
		if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return 0;
		return length;
	}

	// Controls, bidi controls and characters that draw nothing.
	bool is_unsafe_to_show(char32_t c)
	{
		if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return true;
		if (c == 0x00AD || c == 0x061C || c == 0x180E || c == 0xFEFF) return true;
		if (c >= 0x200B && c <= 0x200F) return true;
		if (c >= 0x2028 && c <= 0x202E) return true;
		if (c >= 0x2060 && c <= 0x206F) return true;
		if (c >= 0xFFF9 && c <= 0xFFFB) return true;
		if (c >= 0xE0000 && c <= 0xE007F) return true;
		return false;
	}

	bool is_name_char(char c)
	{
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
	}

	std::string lower(std::string_view text)
	{
		std::string out(text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	// [a-z0-9.-], no empty label, no leading or trailing '-' in a label.
	bool is_valid_host(std::string_view host)
	{
		if (host.empty() || host.size() > 253) return false;
		std::size_t label_start = 0;
		for (std::size_t i = 0; i <= host.size(); ++i) {
			if (i == host.size() || host[i] == '.') {
				const std::string_view label = host.substr(label_start, i - label_start);
				if (label.empty() || label.front() == '-' || label.back() == '-') return false;
				label_start = i + 1;
				continue;
			}
			const char c = host[i];
			const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
			if (!ok) return false;
		}
		return true;
	}

	bool starts_with_nocase(std::string_view text, std::string_view prefix)
	{
		return text.size() >= prefix.size() && lower(text.substr(0, prefix.size())) == prefix;
	}

	edt::forge::forge_error refusal(std::string_view why)
	{
		return edt::forge::make_forge_error(edt::forge::forge_error_code::validation, why);
	}

	// "owner/repo" or "owner/repo.git" into the remote; the host is already checked.
	edt::forge::forge_result<edt::forge::forge_remote> remote_of(std::string host, std::string_view path)
	{
		const std::size_t slash = path.find('/');
		if (slash == std::string_view::npos) return refusal("the remote URL has no owner/repository path");
		std::string_view owner = path.substr(0, slash);
		std::string_view repo = path.substr(slash + 1);
		if (repo.find('/') != std::string_view::npos) return refusal("the remote URL path is not owner/repository");
		if (repo.size() >= 4 && repo.substr(repo.size() - 4) == ".git") repo.remove_suffix(4);
		if (!edt::forge::is_safe_name(owner) || !edt::forge::is_safe_name(repo)) {
			return refusal("the owner or the repository name of the remote URL has unsupported characters");
		}
		return edt::forge::forge_remote{ std::move(host), std::string(owner), std::string(repo) };
	}

	// The host part of `https://host/...` or `ssh://git@host/...`: no userinfo (except the fixed "git@" of ssh), no port.
	edt::forge::forge_result<std::string> checked_host(std::string_view authority, bool ssh)
	{
		if (ssh && authority.starts_with("git@")) authority.remove_prefix(4);
		if (authority.find('@') != std::string_view::npos) return refusal("the remote URL carries a user name or a password");
		if (authority.find(':') != std::string_view::npos) return refusal("a remote URL with a port is not supported");
		std::string host = lower(authority);
		if (!is_valid_host(host)) return refusal("the host of the remote URL is not valid");
		return host;
	}

}

const char* edt::forge::forge_error_code_name(forge_error_code code)
{
	switch (code) {
	case forge_error_code::unauthorized: return "unauthorized";
	case forge_error_code::forbidden: return "forbidden";
	case forge_error_code::rate_limited: return "rate_limited";
	case forge_error_code::not_found: return "not_found";
	case forge_error_code::validation: return "validation";
	case forge_error_code::network: return "network";
	case forge_error_code::invalid_response: return "invalid_response";
	case forge_error_code::host_not_allowed: return "host_not_allowed";
	case forge_error_code::unavailable: return "unavailable";
	}
	return "network";
}

edt::forge::forge_error edt::forge::make_forge_error(forge_error_code code, std::string_view message)
{
	return forge_error{ code, sanitize_text(message, k_max_error_message) };
}

std::string edt::forge::sanitize_text(std::string_view text, std::size_t max_bytes)
{
	std::string out;
	out.reserve(std::min(text.size(), max_bytes) + 3);
	bool cut = false;
	std::size_t i = 0;
	while (i < text.size()) {
		char32_t code = 0;
		std::size_t length = decode_utf8(text.substr(i), code);
		std::string_view piece;
		if (length == 0) {
			length = 1;
			piece = k_replacement;
		} else if (is_unsafe_to_show(code)) {
			piece = " ";
		} else {
			piece = text.substr(i, length);
		}
		if (out.size() + piece.size() > max_bytes) {
			cut = true;
			break;
		}
		out += piece;
		i += length;
	}
	if (cut) {
		// "..." replaces the end; make room for it without splitting a character.
		while (!out.empty() && out.size() + 3 > max_bytes) {
			std::size_t back = out.size() - 1;
			while (back > 0 && (static_cast<unsigned char>(out[back]) & 0xC0) == 0x80) --back;
			out.resize(back);
		}
		out += "...";
	}
	return out;
}

bool edt::forge::is_plain_text(std::string_view text, bool allow_line_breaks)
{
	std::size_t i = 0;
	while (i < text.size()) {
		char32_t code = 0;
		const std::size_t length = decode_utf8(text.substr(i), code);
		if (length == 0) return false;
		const bool line_break = code == U'\t' || code == U'\n' || code == U'\r';
		if (is_unsafe_to_show(code) && !(allow_line_breaks && line_break)) return false;
		i += length;
	}
	return true;
}

bool edt::forge::is_valid_utf8(std::string_view text)
{
	std::size_t i = 0;
	while (i < text.size()) {
		char32_t code = 0;
		const std::size_t length = decode_utf8(text.substr(i), code);
		if (length == 0) return false;
		i += length;
	}
	return true;
}

void edt::forge::scrub_secret(std::string& text, const platform::secret_text& token)
{
	if (token.empty()) return;
	const std::string& secret = token.reveal();
	std::size_t at = 0;
	while ((at = text.find(secret, at)) != std::string::npos) {
		text.replace(at, secret.size(), platform::secret_text::redacted_text);
		at += platform::secret_text::redacted_text.size();
	}
}

bool edt::forge::is_safe_name(std::string_view name)
{
	if (name.empty() || name.size() > 100 || name == "." || name.find("..") != std::string_view::npos) return false;
	return std::all_of(name.begin(), name.end(), is_name_char);
}

bool edt::forge::is_valid_branch_name(std::string_view name)
{
	if (name.empty() || name.size() > 255) return false;
	if (name.front() == '-' || name.front() == '/' || name.back() == '/' || name.back() == '.') return false;
	if (name.find("..") != std::string_view::npos || name.find("@{") != std::string_view::npos
		|| name.find("//") != std::string_view::npos) {
		return false;
	}
	if (name.size() >= 5 && name.substr(name.size() - 5) == ".lock") return false;
	for (const char c : name) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte == 0x7F) return false;
		if (c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\') return false;
	}
	return is_valid_utf8(name);
}

std::optional<std::string> edt::forge::https_url_host(std::string_view url)
{
	for (const char c : url) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte == 0x7F || c == '\\' || c == '"') return std::nullopt;
	}
	if (!starts_with_nocase(url, "https://")) return std::nullopt;
	url.remove_prefix(8);
	const std::size_t end = url.find_first_of("/?#");
	const std::string_view authority = url.substr(0, end);
	if (authority.find('@') != std::string_view::npos || authority.find(':') != std::string_view::npos) return std::nullopt;
	std::string host = lower(authority);
	if (!is_valid_host(host)) return std::nullopt;
	return host;
}

edt::forge::forge_result<edt::forge::forge_remote> edt::forge::parse_forge_remote(std::string_view git_url)
{
	if (git_url.empty() || git_url.size() > 1024) return refusal("the remote URL is empty or too long");
	for (const char c : git_url) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte == 0x7F || c == '\\') return refusal("the remote URL has a space or a control character");
	}
	const bool https = starts_with_nocase(git_url, "https://");
	const bool ssh = starts_with_nocase(git_url, "ssh://");
	if (https || ssh) {
		std::string_view rest = git_url.substr(https ? 8 : 6);
		if (rest.find_first_of("?#") != std::string_view::npos) return refusal("the remote URL has a query or a fragment");
		const std::size_t slash = rest.find('/');
		if (slash == std::string_view::npos) return refusal("the remote URL has no owner/repository path");
		auto host = checked_host(rest.substr(0, slash), ssh);
		if (!host) return host.error();
		return remote_of(std::move(host.value()), rest.substr(slash + 1));
	}
	if (git_url.find("://") != std::string_view::npos) return refusal("only https and ssh remotes are supported");
	if (git_url.starts_with("git@")) {
		const std::string_view rest = git_url.substr(4);
		const std::size_t colon = rest.find(':');
		if (colon == std::string_view::npos) return refusal("the remote URL is not git@host:owner/repository");
		auto host = checked_host(rest.substr(0, colon), false);
		if (!host) return host.error();
		return remote_of(std::move(host.value()), rest.substr(colon + 1));
	}
	return refusal("only https and ssh remotes are supported");
}
