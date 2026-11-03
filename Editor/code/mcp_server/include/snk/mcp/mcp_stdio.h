#pragma once
#include <snk/mcp/mcp_host.h>
#include <snk/mcp/mcp_rpc.h>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

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
		// Writes one message: a compact JSON line with its '\n'. False: the output is broken. Called only from the
		// transport's writer thread, one line at a time; it may block (a client that does not read).
		std::function<bool(std::string_view)> write;
		// The session ended without stop(): the input ended, a line was too long, or the output broke or overflowed.
		// Called once, from one of the transport's threads, after host::session_closed.
		std::function<void()> on_eof;
		std::size_t max_line_bytes = 4 * 1024 * 1024;
		// Lines waiting for the writer. Past this the client is taken as gone (the output is dropped, then on_eof).
		std::size_t max_output_bytes = 64 * 1024 * 1024;
		std::size_t max_subscriptions = 64;
		// How long stop() waits for each of its threads before it leaves it behind (a read or a write that never returns).
		std::chrono::milliseconds stop_timeout{ 2000 };
	};

	// MCP over stdio: newline-delimited JSON-RPC on stdin/stdout, one client, one session (made by initialize). No
	// token and no headers: whoever started the process owns its pipes. The methods are rpc_dispatcher's, as over HTTP;
	// replies and notifications go out through `write` as single lines, from a writer thread of its own, so no thread
	// that answers (the editor's main thread) ever waits for the client. Batches are refused like over HTTP.
	//
	// The host must outlive the transport. After stop() nothing calls the host or on_eof any more, also not a thread
	// that stop() had to leave behind; answers of the host that come later are dropped.
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
		// Closes the session (host::session_closed) if one is open, writes what is queued and ends both threads, each
		// within stop_timeout. No on_eof after it. Safe to call twice.
		void stop();

		// One line of input without its '\n'; a trailing '\r' is dropped. The reader calls it; tests may call it
		// directly when no reader runs. Ignored after stop().
		void handle_line(std::string_view line);

		// Tests: waits until every queued line was written (or the output broke). False after `timeout`.
		bool flush(std::chrono::milliseconds timeout = std::chrono::seconds(10));

		// Empty before initialize and after the session ended.
		std::string session_id() const;

		// Thread-safe. Nothing is written without a session.
		void notify_tools_changed();
		// To the session if it subscribed to `uri`.
		void notify_resource_updated(const std::string& uri);

	private:
		struct core;
		std::shared_ptr<core> m_core;
	};
}
