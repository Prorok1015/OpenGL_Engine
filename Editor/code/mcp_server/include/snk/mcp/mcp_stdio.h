#pragma once
#include <snk/mcp/mcp_host.h>
#include <snk/mcp/mcp_rpc.h>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace mcp
{
	// What the stdio transport reads from: the process' stdin, or a fake in the tests.
	class byte_source
	{
	public:
		virtual ~byte_source() = default;
		// Blocks until bytes arrive and returns how many were put into `buffer`. 0: the input ended (EOF, an error,
		// or interrupt()).
		virtual std::size_t read(char* buffer, std::size_t size) = 0;
		// From another thread: a read that blocks returns 0 soon, and so does every later one. May be called repeatedly.
		virtual void interrupt() = 0;
	};

	// The process' standard input. POSIX: poll() with a short timeout, so interrupt() is seen; Windows: a blocking
	// ReadFile on STD_INPUT_HANDLE that interrupt() cancels with CancelSynchronousIo.
	std::unique_ptr<byte_source> make_stdin_source();

	struct stdio_config
	{
		server_info info;
		// Writes one message: a compact JSON line with its '\n'. The transport never calls it twice at once; it is
		// called from the reader thread and from whatever thread the host answers on.
		std::function<void(std::string_view)> write;
		// The input ended (or a line was too long): called once from the reader thread, after host::session_closed.
		// Not called once stop() has begun.
		std::function<void()> on_eof;
		std::size_t max_line_bytes = 16 * 1024 * 1024;
		std::size_t max_subscriptions = 64;
	};

	// MCP over stdio: newline-delimited JSON-RPC on stdin/stdout, one client, one session (made by initialize). No
	// token and no headers: whoever started the process owns its pipes. The methods are rpc_dispatcher's, as over HTTP;
	// replies and notifications go out through `write` as single lines. Batches are refused like over HTTP.
	//
	// The host must outlive the transport. Answers of the host that come after the transport is gone are still
	// written (the output is shared with them), so `write` must stay callable as long as the host may answer.
	class stdio_transport
	{
	public:
		stdio_transport(host& application, stdio_config config);
		// stop()
		~stdio_transport();
		stdio_transport(const stdio_transport&) = delete;
		stdio_transport& operator=(const stdio_transport&) = delete;

		// Starts the reader thread over `source`. Once.
		void start(std::unique_ptr<byte_source> source);
		// Interrupts the source, joins the reader and closes the session (host::session_closed) if one is open. No
		// on_eof after it. Safe to call twice.
		void stop();

		// One line of input without its '\n'; a trailing '\r' is dropped. The reader calls it; tests may call it
		// directly when no reader runs.
		void handle_line(std::string_view line);
		// The input ended: closes the session and calls on_eof (unless stopping). The reader calls it.
		void end_of_input();

		// Empty before initialize and after the input ended.
		std::string session_id() const;

		// Thread-safe. Nothing is written without a session.
		void notify_tools_changed();
		// To the session if it subscribed to `uri`.
		void notify_resource_updated(const std::string& uri);

	private:
		struct output;
		struct session_state;

		void read_loop();
		void send(const boost::json::value& message);

		host& m_host;
		stdio_config m_config;
		rpc_dispatcher m_rpc;
		std::shared_ptr<output> m_output;
		std::shared_ptr<session_state> m_session;

		std::unique_ptr<byte_source> m_source;
		std::thread m_reader;
		std::mutex m_reader_mutex;
		std::condition_variable m_reader_cv;
		bool m_reader_done = false;
		bool m_stopping = false;
		bool m_eof_handled = false;
	};
}
