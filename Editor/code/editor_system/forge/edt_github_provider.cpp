#include "edt_github_provider.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/stream_parser.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

namespace json = boost::json;

namespace {

	using edt::forge::forge_error;
	using edt::forge::forge_error_code;
	using edt::forge::forge_result;
	using edt::forge::pr_info;

	constexpr std::size_t k_max_response = 1024 * 1024;
	constexpr std::size_t k_max_title = 256;
	constexpr std::size_t k_max_body = 64 * 1024;
	constexpr std::size_t k_text_limit = 512;

	struct target
	{
		// https://api.github.com/repos/owner/repo
		std::string repo_url;
		// Where html_url must point.
		std::string web_host;
	};

	std::string lower(std::string_view text)
	{
		std::string out(text);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	forge_error error_of(forge_error_code code, std::string_view message)
	{
		return edt::forge::make_forge_error(code, message);
	}

	std::string percent_encode(std::string_view text)
	{
		static constexpr char hex[] = "0123456789ABCDEF";
		std::string out;
		out.reserve(text.size());
		for (const char c : text) {
			const auto byte = static_cast<unsigned char>(c);
			if (std::isalnum(byte) != 0 || c == '-' || c == '.' || c == '_' || c == '~') {
				out += c;
			} else {
				out += '%';
				out += hex[byte >> 4];
				out += hex[byte & 0x0F];
			}
		}
		return out;
	}

	forge_result<target> target_of(const edt::forge::forge_remote& remote, const edt::forge::github_options& options)
	{
		if (!edt::forge::is_safe_name(remote.owner) || !edt::forge::is_safe_name(remote.repo)) {
			return error_of(forge_error_code::validation, "the owner or the repository name is not valid");
		}
		std::string base;
		std::string web_host;
		const std::string host = lower(remote.host);
		if (host == "github.com") {
			base = "https://api.github.com";
			web_host = "github.com";
		} else if (!options.api_base.empty() && !options.web_host.empty() && host == lower(options.web_host)) {
			base = options.api_base;
			while (!base.empty() && base.back() == '/') base.pop_back();
			if (!edt::forge::https_url_host(base)) return error_of(forge_error_code::host_not_allowed, "the configured GitHub API base is not an https URL");
			web_host = host;
		} else {
			return error_of(forge_error_code::host_not_allowed, std::format("'{}' is not a supported GitHub host", host));
		}
		return target{ std::format("{}/repos/{}/{}", base, remote.owner, remote.repo), std::move(web_host) };
	}

	edt::forge::http_request make_request(std::string method, std::string url, const platform::secret_text& token)
	{
		edt::forge::http_request request;
		request.method = std::move(method);
		request.url = std::move(url);
		request.headers = {
			{ "Accept", "application/vnd.github+json" },
			{ "X-GitHub-Api-Version", "2022-11-28" },
			{ "User-Agent", "snk-editor" },
		};
		request.bearer_token = &token;
		return request;
	}

	// Parses within limits; nullopt-like failure through the error.
	forge_result<json::value> parse_body(const std::string& body)
	{
		if (body.size() > k_max_response) return error_of(forge_error_code::invalid_response, "the response is too large");
		json::parse_options options;
		options.max_depth = 16;
		json::stream_parser parser({}, options);
		boost::system::error_code ec;
		parser.write(body.data(), body.size(), ec);
		if (!ec) parser.finish(ec);
		if (ec) return error_of(forge_error_code::invalid_response, "the response is not valid JSON");
		return parser.release();
	}

	const json::value* find(const json::object& object, std::string_view key)
	{
		const auto it = object.find(key);
		return it == object.end() ? nullptr : &it->value();
	}

	// The server's own message for an error answer; empty when there is none.
	std::string server_message(const std::string& body)
	{
		auto parsed = parse_body(body);
		if (!parsed.ok() || !parsed.value().is_object()) return {};
		const json::object& object = parsed.value().as_object();
		std::string out;
		if (const json::value* message = find(object, "message"); message != nullptr && message->is_string()) {
			out = std::string(message->as_string());
		}
		if (const json::value* errors = find(object, "errors"); errors != nullptr && errors->is_array()) {
			int shown = 0;
			for (const json::value& item : errors->as_array()) {
				if (shown == 3) break;
				const json::value* text = nullptr;
				if (item.is_string()) {
					text = &item;
				} else if (item.is_object()) {
					text = find(item.as_object(), "message");
				}
				if (text == nullptr || !text->is_string()) continue;
				out += out.empty() ? "" : "; ";
				out += std::string(text->as_string());
				++shown;
			}
		}
		return out;
	}

	forge_error status_error(int status, const std::string& body, const platform::secret_text& token)
	{
		std::string server = server_message(body);
		edt::forge::scrub_secret(server, token);
		server = edt::forge::sanitize_text(server, 200);
		const auto with = [&](std::string_view what) {
			return server.empty() ? std::string(what) : std::format("{}: {}", what, server);
		};
		const std::string folded = lower(server);
		switch (status) {
		case 401: return error_of(forge_error_code::unauthorized, with("GitHub rejected the token"));
		case 403:
			if (folded.find("rate limit") != std::string::npos) return error_of(forge_error_code::rate_limited, with("GitHub rate limit reached"));
			return error_of(forge_error_code::forbidden, with("GitHub refused the request"));
		case 404: return error_of(forge_error_code::not_found, with("GitHub did not find it (or the token cannot see it)"));
		case 422: return error_of(forge_error_code::validation, with("GitHub did not accept the request"));
		case 429: return error_of(forge_error_code::rate_limited, with("GitHub rate limit reached"));
		default: break;
		}
		if (status >= 500) return error_of(forge_error_code::network, std::format("GitHub failed (HTTP {})", status));
		return error_of(forge_error_code::invalid_response, std::format("unexpected HTTP {} from GitHub", status));
	}

	// Transport failure, an error status, or ok (nullopt).
	std::optional<forge_error> check(const edt::forge::http_response& response, const platform::secret_text& token)
	{
		if (!response.error_text.empty()) {
			std::string text = response.error_text;
			edt::forge::scrub_secret(text, token);
			return error_of(response.error_code, text);
		}
		if (response.status >= 200 && response.status < 300) return std::nullopt;
		return status_error(response.status, response.body, token);
	}

	// Safe text out of the server's string; nullopt when the field is missing or not a string.
	std::optional<std::string> text_field(const json::object& object, std::string_view key, const platform::secret_text& token,
		std::size_t max = k_text_limit)
	{
		const json::value* value = find(object, key);
		if (value == nullptr || !value->is_string()) return std::nullopt;
		std::string text(value->as_string());
		edt::forge::scrub_secret(text, token);
		return edt::forge::sanitize_text(text, max);
	}

	std::optional<std::string> ref_of(const json::object& object, std::string_view key, const platform::secret_text& token)
	{
		const json::value* side = find(object, key);
		if (side == nullptr || !side->is_object()) return std::nullopt;
		return text_field(side->as_object(), "ref", token, 255);
	}

	forge_result<pr_info> pr_from(const json::value& value, std::string_view web_host, const platform::secret_text& token)
	{
		const auto bad = [](std::string_view what) {
			return error_of(forge_error_code::invalid_response, std::format("the pull request has no valid {}", what));
		};
		if (!value.is_object()) return bad("object");
		const json::object& object = value.as_object();
		pr_info info;
		const json::value* number = find(object, "number");
		if (number == nullptr || !number->is_int64() || number->as_int64() <= 0) return bad("number");
		info.number = number->as_int64();
		auto title = text_field(object, "title", token);
		if (!title) return bad("title");
		info.title = std::move(*title);
		auto head = ref_of(object, "head", token);
		if (!head) return bad("head");
		info.head = std::move(*head);
		auto base = ref_of(object, "base", token);
		if (!base) return bad("base");
		info.base = std::move(*base);

		const auto state = text_field(object, "state", token);
		if (!state || (*state != "open" && *state != "closed")) return bad("state");
		const json::value* merged = find(object, "merged");
		const json::value* merged_at = find(object, "merged_at");
		const bool is_merged = (merged != nullptr && merged->is_bool() && merged->as_bool())
			|| (merged_at != nullptr && merged_at->is_string());
		info.state = *state == "open" ? "open" : (is_merged ? "merged" : "closed");
		const json::value* draft = find(object, "draft");
		info.draft = draft != nullptr && draft->is_bool() && draft->as_bool();

		if (const json::value* url = find(object, "html_url"); url != nullptr && url->is_string()) {
			const std::string text(url->as_string());
			const auto host = edt::forge::https_url_host(text);
			if (host && *host == web_host && text.size() <= k_text_limit) {
				std::string safe = text;
				edt::forge::scrub_secret(safe, token);
				info.url = std::move(safe);
			}
		}
		return info;
	}

}

edt::forge::github_provider::github_provider(http_transport transport, github_options options)
	: m_transport(std::move(transport)), m_options(std::move(options))
{
}

edt::forge::forge_result<edt::forge::pr_info> edt::forge::github_provider::create_pr(const forge_remote& remote,
	const create_pr_request& request, const platform::secret_text& token, std::stop_token stop)
{
	auto where = target_of(remote, m_options);
	if (!where) return where.error();
	if (!is_valid_branch_name(request.head) || !is_valid_branch_name(request.base)) {
		return error_of(forge_error_code::validation, "the head or the base branch name is not valid");
	}
	if (request.title.empty() || request.title.size() > k_max_title || !is_valid_utf8(request.title)) {
		return error_of(forge_error_code::validation, "the title is empty, too long or not valid UTF-8");
	}
	if (request.body.size() > k_max_body || !is_valid_utf8(request.body)) {
		return error_of(forge_error_code::validation, "the description is too long or not valid UTF-8");
	}

	json::object payload;
	payload["title"] = json::string(request.title);
	payload["head"] = json::string(request.head);
	payload["base"] = json::string(request.base);
	payload["body"] = json::string(request.body);
	payload["draft"] = request.draft;

	http_request http = make_request("POST", where.value().repo_url + "/pulls", token);
	http.headers.emplace_back("Content-Type", "application/json");
	http.body = json::serialize(payload);
	const http_response response = m_transport(http, stop);
	if (auto failed = check(response, token)) return *failed;
	auto parsed = parse_body(response.body);
	if (!parsed) return parsed.error();
	return pr_from(parsed.value(), where.value().web_host, token);
}

edt::forge::forge_result<edt::forge::pr_info> edt::forge::github_provider::pr_status(const forge_remote& remote,
	std::int64_t number, const platform::secret_text& token, std::stop_token stop)
{
	auto where = target_of(remote, m_options);
	if (!where) return where.error();
	if (number <= 0) return error_of(forge_error_code::validation, "the pull request number is not valid");

	const http_request http = make_request("GET", std::format("{}/pulls/{}", where.value().repo_url, number), token);
	const http_response response = m_transport(http, stop);
	if (auto failed = check(response, token)) return *failed;
	auto parsed = parse_body(response.body);
	if (!parsed) return parsed.error();
	return pr_from(parsed.value(), where.value().web_host, token);
}

edt::forge::forge_result<std::vector<edt::forge::pr_info>> edt::forge::github_provider::list_prs(const forge_remote& remote,
	const list_prs_filter& filter, const platform::secret_text& token, std::stop_token stop)
{
	auto where = target_of(remote, m_options);
	if (!where) return where.error();
	if (filter.state != "open" && filter.state != "closed" && filter.state != "all") {
		return error_of(forge_error_code::validation, "the state filter is not open, closed or all");
	}
	std::string query = std::format("state={}&per_page={}&page=1", filter.state, std::clamp(filter.limit, 1, 100));
	if (filter.head) {
		// GitHub filters by `user:branch`.
		std::string head = *filter.head;
		const std::size_t colon = head.find(':');
		const std::string owner = colon == std::string::npos ? remote.owner : head.substr(0, colon);
		const std::string branch = colon == std::string::npos ? head : head.substr(colon + 1);
		if (!is_safe_name(owner) || !is_valid_branch_name(branch)) {
			return error_of(forge_error_code::validation, "the head filter is not valid");
		}
		query += "&head=" + percent_encode(owner + ":" + branch);
	}
	if (filter.base) {
		if (!is_valid_branch_name(*filter.base)) return error_of(forge_error_code::validation, "the base filter is not valid");
		query += "&base=" + percent_encode(*filter.base);
	}

	const http_request http = make_request("GET", where.value().repo_url + "/pulls?" + query, token);
	const http_response response = m_transport(http, stop);
	if (auto failed = check(response, token)) return *failed;
	auto parsed = parse_body(response.body);
	if (!parsed) return parsed.error();
	if (!parsed.value().is_array()) return error_of(forge_error_code::invalid_response, "the response is not a list");
	const json::array& items = parsed.value().as_array();
	std::vector<pr_info> out;
	out.reserve(std::min<std::size_t>(items.size(), 100));
	for (const json::value& item : items) {
		if (out.size() >= 100) break;
		auto info = pr_from(item, where.value().web_host, token);
		if (!info) return info.error();
		out.push_back(std::move(info.value()));
	}
	return out;
}
