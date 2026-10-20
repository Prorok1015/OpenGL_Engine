#include <snk/mcp/mcp_http.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
	constexpr std::size_t max_content_length_digits = 18;

	char lower(char c)
	{
		return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
	}

	std::string lower_copy(std::string_view text)
	{
		std::string out(text);
		for (char& c : out) {
			c = lower(c);
		}
		return out;
	}

	bool is_token_char(char c)
	{
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
			return true;
		}
		switch (c) {
		case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
		case '-': case '.': case '^': case '_': case '`': case '|': case '~':
			return true;
		default:
			return false;
		}
	}

	std::string_view trim(std::string_view text)
	{
		while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
			text.remove_prefix(1);
		}
		while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
			text.remove_suffix(1);
		}
		return text;
	}

	bool has_forbidden_byte(std::string_view line)
	{
		for (const char c : line) {
			if (c == '\r' || c == '\n' || c == '\0') {
				return true;
			}
		}
		return false;
	}

	// True when the comma separated header value lists `token` (case-insensitive).
	bool lists_token(std::string_view value, std::string_view token)
	{
		std::size_t begin = 0;
		while (begin <= value.size()) {
			std::size_t end = value.find(',', begin);
			if (end == std::string_view::npos) {
				end = value.size();
			}
			if (lower_copy(trim(value.substr(begin, end - begin))) == token) {
				return true;
			}
			begin = end + 1;
		}
		return false;
	}
}

const std::string* mcp::http_request::header(std::string_view lower_name) const
{
	for (const http_header& h : headers) {
		if (h.name == lower_name) {
			return &h.value;
		}
	}
	return nullptr;
}

mcp::event_channel::event_channel(std::size_t max_pending)
	: m_max_pending(max_pending)
{
}

bool mcp::event_channel::push(std::string frame)
{
	std::function<void()> wake;
	bool accepted = false;
	{
		std::lock_guard lock(m_mutex);
		if (!m_closed) {
			if (m_frames.size() >= m_max_pending) {
				m_closed = true;
			} else {
				m_frames.push_back(std::move(frame));
				accepted = true;
			}
			wake = m_wake;
		}
	}
	if (wake) {
		wake();
	}
	return accepted;
}

std::vector<std::string> mcp::event_channel::take()
{
	std::lock_guard lock(m_mutex);
	std::vector<std::string> out;
	out.reserve(m_frames.size());
	for (std::string& frame : m_frames) {
		out.push_back(std::move(frame));
	}
	m_frames.clear();
	return out;
}

void mcp::event_channel::close()
{
	std::function<void()> wake;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed) {
			return;
		}
		m_closed = true;
		wake = m_wake;
	}
	if (wake) {
		wake();
	}
}

bool mcp::event_channel::closed() const
{
	std::lock_guard lock(m_mutex);
	return m_closed;
}

void mcp::event_channel::set_wake(std::function<void()> wake)
{
	std::function<void()> call;
	{
		std::lock_guard lock(m_mutex);
		m_wake = std::move(wake);
		if (m_wake && (m_closed || !m_frames.empty())) {
			call = m_wake;
		}
	}
	if (call) {
		call();
	}
}

void mcp::http_response::set_header(std::string name, std::string value)
{
	for (http_header& h : headers) {
		if (lower_copy(h.name) == lower_copy(name)) {
			h.value = std::move(value);
			return;
		}
	}
	headers.push_back({ std::move(name), std::move(value) });
}

std::string_view mcp::reason_phrase(int status)
{
	switch (status) {
	case 100: return "Continue";
	case 200: return "OK";
	case 202: return "Accepted";
	case 204: return "No Content";
	case 400: return "Bad Request";
	case 401: return "Unauthorized";
	case 403: return "Forbidden";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	case 406: return "Not Acceptable";
	case 413: return "Payload Too Large";
	case 415: return "Unsupported Media Type";
	case 431: return "Request Header Fields Too Large";
	case 500: return "Internal Server Error";
	case 501: return "Not Implemented";
	case 503: return "Service Unavailable";
	case 505: return "HTTP Version Not Supported";
	default: return "Status";
	}
}

namespace {
	void append_head(std::string& out, const mcp::http_response& response)
	{
		out += "HTTP/1.1 ";
		out += std::to_string(response.status);
		out += ' ';
		out += mcp::reason_phrase(response.status);
		out += "\r\n";
		for (const mcp::http_header& h : response.headers) {
			out += h.name;
			out += ": ";
			out += h.value;
			out += "\r\n";
		}
	}
}

std::string mcp::serialize_response(const http_response& response)
{
	std::string out;
	out.reserve(256 + response.body.size());
	append_head(out, response);
	out += "Content-Length: ";
	out += std::to_string(response.body.size());
	out += "\r\n";
	if (response.close) {
		out += "Connection: close\r\n";
	}
	out += "\r\n";
	out += response.body;
	return out;
}

std::string mcp::serialize_stream_head(const http_response& response)
{
	std::string out;
	append_head(out, response);
	out += "Connection: keep-alive\r\n\r\n";
	return out;
}

std::string mcp::sse_message(std::string_view json)
{
	std::string out;
	out.reserve(json.size() + 24);
	out += "event: message\ndata: ";
	out += json;
	out += "\n\n";
	return out;
}

mcp::http_request_parser::http_request_parser(http_limits limits)
	: m_limits(limits)
{
}

bool mcp::http_request_parser::fail(int status, std::string message)
{
	m_failed = true;
	m_error_status = status;
	m_error_message = std::move(message);
	return false;
}

mcp::http_request_parser::state mcp::http_request_parser::feed(std::string_view data)
{
	if (m_failed) {
		return state::error;
	}
	m_buffer.append(data);
	if (!m_headers_done) {
		const std::size_t head_end = m_buffer.find("\r\n\r\n");
		if (head_end == std::string::npos) {
			if (m_buffer.size() > m_limits.max_header_bytes) {
				fail(431, "request headers are too large");
				return state::error;
			}
			return state::need_more;
		}
		if (head_end + 4 > m_limits.max_header_bytes) {
			fail(431, "request headers are too large");
			return state::error;
		}
		if (!parse_head(head_end)) {
			return state::error;
		}
	}
	if (m_buffer.size() - m_body_start < m_body_length) {
		return state::need_more;
	}
	return state::complete;
}

bool mcp::http_request_parser::parse_head(std::size_t head_end)
{
	const std::string_view head(m_buffer.data(), head_end);
	http_request request;

	std::size_t line_begin = 0;
	bool first_line = true;
	bool have_length = false;
	std::size_t content_length = 0;
	bool http_1_1 = true;
	while (line_begin <= head.size()) {
		std::size_t line_end = head.find("\r\n", line_begin);
		if (line_end == std::string_view::npos) {
			line_end = head.size();
		}
		const std::string_view line = head.substr(line_begin, line_end - line_begin);
		line_begin = line_end + 2;

		if (has_forbidden_byte(line)) {
			return fail(400, "a control character in the request head");
		}
		if (first_line) {
			first_line = false;
			const std::size_t first_space = line.find(' ');
			const std::size_t second_space = first_space == std::string_view::npos ? first_space : line.find(' ', first_space + 1);
			if (first_space == std::string_view::npos || second_space == std::string_view::npos) {
				return fail(400, "malformed request line");
			}
			const std::string_view method = line.substr(0, first_space);
			const std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
			const std::string_view version = line.substr(second_space + 1);
			if (method.empty() || !std::all_of(method.begin(), method.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) {
				return fail(400, "malformed request method");
			}
			if (target.empty() || target.find(' ') != std::string_view::npos || (target.front() != '/' && target != "*")) {
				return fail(400, "malformed request target");
			}
			if (version == "HTTP/1.1") {
				http_1_1 = true;
			} else if (version == "HTTP/1.0") {
				http_1_1 = false;
			} else if (version.substr(0, 5) == "HTTP/") {
				return fail(505, "only HTTP/1.0 and HTTP/1.1 are supported");
			} else {
				return fail(400, "malformed request line");
			}
			request.method = std::string(method);
			request.target = std::string(target);
			continue;
		}

		if (line.empty()) {
			return fail(400, "an empty line inside the request headers");
		}
		if (line.front() == ' ' || line.front() == '\t') {
			return fail(400, "folded header lines are not supported");
		}
		const std::size_t colon = line.find(':');
		if (colon == std::string_view::npos || colon == 0) {
			return fail(400, "malformed header line");
		}
		const std::string_view name = line.substr(0, colon);
		if (!std::all_of(name.begin(), name.end(), is_token_char)) {
			return fail(400, "malformed header name");
		}
		if (request.headers.size() >= m_limits.max_headers) {
			return fail(431, "too many request headers");
		}
		request.headers.push_back({ lower_copy(name), std::string(trim(line.substr(colon + 1))) });
	}

	if (request.header("transfer-encoding") != nullptr) {
		return fail(501, "Transfer-Encoding is not supported, send Content-Length");
	}
	for (const http_header& h : request.headers) {
		if (h.name != "content-length") {
			continue;
		}
		if (h.value.empty() || h.value.size() > max_content_length_digits
			|| !std::all_of(h.value.begin(), h.value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
			return fail(400, "malformed Content-Length");
		}
		const std::size_t value = static_cast<std::size_t>(std::stoull(h.value));
		if (have_length && value != content_length) {
			return fail(400, "conflicting Content-Length headers");
		}
		have_length = true;
		content_length = value;
	}
	if (content_length > m_limits.max_body_bytes) {
		return fail(413, "the request body is too large");
	}

	request.keep_alive = http_1_1;
	if (const std::string* connection = request.header("connection")) {
		if (lists_token(*connection, "close")) {
			request.keep_alive = false;
		} else if (lists_token(*connection, "keep-alive")) {
			request.keep_alive = true;
		}
	}
	if (const std::string* expect = request.header("expect")) {
		m_expect_continue = lower_copy(*expect) == "100-continue";
	}

	m_request = std::move(request);
	m_body_start = head_end + 4;
	m_body_length = content_length;
	m_headers_done = true;
	return true;
}

mcp::http_request mcp::http_request_parser::take()
{
	http_request out = std::move(m_request);
	out.body.assign(m_buffer, m_body_start, m_body_length);
	m_buffer.erase(0, m_body_start + m_body_length);
	m_request = http_request{};
	m_headers_done = false;
	m_expect_continue = false;
	m_body_start = 0;
	m_body_length = 0;
	return out;
}
