#pragma once
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace mcp
{
	struct http_header
	{
		std::string name; // lower case in a request
		std::string value;
	};

	struct http_request
	{
		std::string method;
		std::string target;
		std::vector<http_header> headers;
		std::string body;
		bool keep_alive = true;

		// `lower_name` is lower case. Null when the header is absent; the first one wins.
		const std::string* header(std::string_view lower_name) const;
	};

	// The server-to-client event stream of one session (Server-Sent Events). Thread-safe: the endpoint pushes
	// from any thread, the connection takes the frames on the I/O thread when it is woken.
	class event_channel
	{
	public:
		explicit event_channel(std::size_t max_pending = 256);

		// False when the channel is closed or full. A client that does not read falls behind, and the channel
		// closes (the connection is dropped) instead of growing without a bound.
		bool push(std::string frame);
		std::vector<std::string> take();
		void close();
		bool closed() const;
		// Called (outside the lock) after a push or the close, and at once if something is already waiting.
		void set_wake(std::function<void()> wake);

	private:
		mutable std::mutex m_mutex;
		std::deque<std::string> m_frames;
		std::function<void()> m_wake;
		bool m_closed = false;
		std::size_t m_max_pending;
	};

	struct http_response
	{
		int status = 200;
		std::vector<http_header> headers;
		std::string body;
		// Close the connection after this response.
		bool close = false;
		// Set: the headers are those of an event stream and the connection then sends what the channel yields.
		std::shared_ptr<event_channel> stream;

		void set_header(std::string name, std::string value);
	};

	std::string_view reason_phrase(int status);
	// Status line, headers, Content-Length and the body.
	std::string serialize_response(const http_response& response);
	// Status line and headers of an event stream (no Content-Length), ending with the blank line.
	std::string serialize_stream_head(const http_response& response);
	// "event: message\ndata: <json>\n\n"; `json` must be on one line.
	std::string sse_message(std::string_view json);

	struct http_limits
	{
		std::size_t max_header_bytes = 16 * 1024; // request line and headers together
		std::size_t max_headers = 64;
		std::size_t max_body_bytes = 1024 * 1024;
	};

	// Incremental HTTP/1.1 request parser: bytes in, requests out. Understands Content-Length bodies, keep-alive and
	// pipelining; refuses chunked bodies (501) and anything malformed (400) or over the limits (431, 413).
	class http_request_parser
	{
	public:
		enum class state { need_more, complete, error };

		explicit http_request_parser(http_limits limits = {});

		// Appends `data` (may be empty) and parses as far as it can.
		state feed(std::string_view data);
		// After `complete`: the request. Bytes behind it stay for the next one; feed({}) parses them.
		http_request take();
		// Headers are complete, the body is not, and the client waits for "100 Continue".
		bool expects_continue() const { return m_headers_done && m_expect_continue; }

		int error_status() const { return m_error_status; }
		const std::string& error_message() const { return m_error_message; }

	private:
		bool parse_head(std::size_t head_end);
		bool fail(int status, std::string message);

		http_limits m_limits;
		std::string m_buffer;
		http_request m_request;
		bool m_headers_done = false;
		bool m_expect_continue = false;
		bool m_failed = false;
		std::size_t m_body_start = 0;
		std::size_t m_body_length = 0;
		int m_error_status = 0;
		std::string m_error_message;
	};
}
