#pragma once
#include <snk/platform/plt_secret.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// Types of the forge layer (#458): the remote of a repository, pull requests and errors. Pure; nothing here touches the
// network. Text that comes from a server goes through sanitize_text before it reaches an error or a pr_info.
namespace edt::forge
{
	enum class forge_error_code : std::uint8_t
	{
		unauthorized,
		forbidden,
		rate_limited,
		not_found,
		validation,
		network,
		invalid_response,
		host_not_allowed,
		unavailable,
	};

	const char* forge_error_code_name(forge_error_code code);

	// Longest message of a forge_error, in bytes.
	inline constexpr std::size_t k_max_error_message = 300;

	struct forge_error
	{
		forge_error_code code = forge_error_code::network;
		// Safe to show and to log: no token, no control or invisible characters, at most k_max_error_message bytes.
		std::string message;
	};

	// A forge_error whose message went through sanitize_text.
	forge_error make_forge_error(forge_error_code code, std::string_view message);

	// A value or a forge_error.
	template <class T>
	class forge_result
	{
	public:
		forge_result(T value) : m_state(std::move(value)) {}
		forge_result(forge_error error) : m_state(std::move(error)) {}

		bool ok() const { return std::holds_alternative<T>(m_state); }
		explicit operator bool() const { return ok(); }
		T& value() { return std::get<T>(m_state); }
		const T& value() const { return std::get<T>(m_state); }
		const forge_error& error() const { return std::get<forge_error>(m_state); }

	private:
		std::variant<T, forge_error> m_state;
	};

	// `text` safe to display: invalid UTF-8 becomes U+FFFD, control characters (C0, DEL, C1), bidi controls and
	// invisible characters (zero width, soft hyphen, BOM, ...) become a space, then it is cut to `max_bytes` at a
	// character boundary ("..." at the end when cut).
	std::string sanitize_text(std::string_view text, std::size_t max_bytes);
	bool is_valid_utf8(std::string_view text);
	// Replaces every occurrence of the token in `text` with "<redacted>"; no-op for an empty token.
	void scrub_secret(std::string& text, const platform::secret_text& token);

	// [A-Za-z0-9._-], not empty, no "..", not "." (a repository owner or name).
	bool is_safe_name(std::string_view name);
	// A branch name git would accept, without anything that could change a URL or a query by accident: not empty, at
	// most 255 bytes, no control character, space or any of ~^:?*[\ , no "..", "@{", "//", no leading '-' or '/', no
	// trailing '/', '.' or ".lock".
	bool is_valid_branch_name(std::string_view name);
	// The lowercased host of an `https://host/path` URL; nullopt for any other scheme, userinfo, a port, an empty or
	// malformed host, a space or control character anywhere.
	std::optional<std::string> https_url_host(std::string_view url);

	struct forge_remote
	{
		// Lowercase.
		std::string host;
		std::string owner;
		std::string repo;
	};

	// `https://host/owner/repo[.git]`, `git@host:owner/repo[.git]`, `ssh://git@host/owner/repo[.git]`. Refused (validation):
	// other schemes, userinfo in an https URL, a port, owner or repo outside [A-Za-z0-9._-] or holding "..", a longer
	// path, a query or a fragment.
	forge_result<forge_remote> parse_forge_remote(std::string_view git_url);

	struct pr_info
	{
		std::int64_t number = 0;
		std::string title;
		// "open", "closed" or "merged".
		std::string state;
		bool draft = false;
		// Branch names.
		std::string head;
		std::string base;
		// Empty when the server gave none or one of another host.
		std::string url;
	};

	struct create_pr_request
	{
		std::string base;
		std::string head;
		std::string title;
		std::string body;
		bool draft = true;
	};

	struct list_prs_filter
	{
		// "open", "closed" or "all".
		std::string state = "open";
		// Branch names (head may also be `owner:branch`).
		std::optional<std::string> head;
		std::optional<std::string> base;
		// 1..100.
		int limit = 30;
	};
}
