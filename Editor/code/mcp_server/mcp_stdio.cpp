#include <snk/mcp/mcp_stdio.h>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

namespace json = boost::json;

namespace {
	using mcp::rpc_error;
	using mcp::rpc_codes::internal_error;
	using mcp::rpc_codes::invalid_params;
	using mcp::rpc_codes::invalid_request;
	using mcp::rpc_codes::parse_error;

	constexpr std::size_t read_chunk = 64 * 1024;

#ifdef _WIN32
	class stdin_source final : public mcp::byte_source
	{
	public:
		~stdin_source() override
		{
			if (m_thread != nullptr) {
				CloseHandle(m_thread);
			}
		}

		std::size_t read(char* buffer, std::size_t size) override
		{
			const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
			if (input == nullptr || input == INVALID_HANDLE_VALUE) {
				return 0;
			}
			// A pipe can carry a write of zero bytes; only a console or a file reports its end that way.
			const bool is_pipe = GetFileType(input) == FILE_TYPE_PIPE;
			for (;;) {
				{
					std::lock_guard lock(m_mutex);
					if (m_stop) {
						return 0;
					}
					if (m_thread == nullptr) {
						HANDLE self = nullptr;
						if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
							m_thread = self;
						}
					}
				}
				DWORD got = 0;
				const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(size, read_chunk));
				if (ReadFile(input, buffer, wanted, &got, nullptr)) {
					if (got > 0 || !is_pipe) {
						return static_cast<std::size_t>(got);
					}
					continue;
				}
				const DWORD error = GetLastError();
				if (error == ERROR_OPERATION_ABORTED) {
					// Cancelled by interrupt(), or by someone else: then read on.
					std::lock_guard lock(m_mutex);
					if (m_stop) {
						return 0;
					}
					continue;
				}
				// ERROR_BROKEN_PIPE, ERROR_HANDLE_EOF: the writer is gone. Anything else cannot be read past either.
				return 0;
			}
		}

		void interrupt() override
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
			if (m_thread != nullptr) {
				CancelSynchronousIo(m_thread);
			}
		}

	private:
		std::mutex m_mutex;
		HANDLE m_thread = nullptr;
		bool m_stop = false;
	};
#else
	class stdin_source final : public mcp::byte_source
	{
	public:
		std::size_t read(char* buffer, std::size_t size) override
		{
			while (!m_stop.load()) {
				pollfd watched{ STDIN_FILENO, POLLIN, 0 };
				const int ready = ::poll(&watched, 1, 100);
				if (ready < 0) {
					if (errno == EINTR) {
						continue;
					}
					return 0;
				}
				if (ready == 0) {
					continue;
				}
				const ssize_t got = ::read(STDIN_FILENO, buffer, size);
				if (got < 0) {
					if (errno == EINTR || errno == EAGAIN) {
						continue;
					}
					return 0;
				}
				return static_cast<std::size_t>(got);
			}
			return 0;
		}

		void interrupt() override
		{
			m_stop = true;
		}

	private:
		std::atomic<bool> m_stop{ false };
	};
#endif
}

std::unique_ptr<mcp::byte_source> mcp::make_stdin_source()
{
	return std::make_unique<stdin_source>();
}

// Everything the threads touch, so a thread that stop() leaves behind still finds it. Lock order: gate, then
// session_mutex, then out_mutex.
struct mcp::stdio_transport::core : std::enable_shared_from_this<mcp::stdio_transport::core>
{
	core(host& application_, stdio_config config_)
		: application(application_)
		, config(std::move(config_))
		, rpc(application_, config.info)
	{
	}

	host& application;
	const stdio_config config;
	rpc_dispatcher rpc;

	// Held while the host or on_eof is called for the transport. Once `stopping` is set under it, neither is called again.
	std::mutex gate;
	bool stopping = false;
	bool ended = false;

	mutable std::mutex session_mutex;
	std::string session_id;
	std::set<std::string> subscriptions;

	std::mutex out_mutex;
	std::condition_variable out_cv;
	std::deque<std::string> queue;
	std::size_t queued_bytes = 0;
	bool writing = false;
	bool out_broken = false;
	bool out_closing = false;

	std::unique_ptr<byte_source> source;
	std::thread reader;
	std::thread writer;
	std::mutex threads_mutex;
	std::condition_variable threads_cv;
	bool reader_done = false;
	bool writer_done = false;

	void enqueue(std::string line)
	{
		bool overflowed = false;
		{
			std::lock_guard lock(out_mutex);
			if (out_broken || out_closing) {
				return;
			}
			if (queued_bytes + line.size() > config.max_output_bytes) {
				out_broken = true;
				overflowed = true;
				queue.clear();
				queued_bytes = 0;
			} else {
				queued_bytes += line.size();
				queue.push_back(std::move(line));
			}
		}
		out_cv.notify_all();
		if (overflowed) {
			// The writer may stay blocked for good, and the caller may hold the gate or the session lock: the session is
			// ended from a thread of its own (once: out_broken is never cleared).
			std::thread([self = shared_from_this()] {
				self->end_session("the MCP stdio client does not read its output (over the limit): the session ends");
			}).detach();
		}
	}

	void send(const json::value& message)
	{
		std::string line = json::serialize(message);
		line.push_back('\n');
		enqueue(std::move(line));
	}

	// Only while `session` is still the open one: an answer to a session that was replaced or closed is dropped.
	void send_to(const std::string& session, const json::value& message)
	{
		std::string line = json::serialize(message);
		line.push_back('\n');
		std::lock_guard lock(session_mutex);
		if (session_id == session) {
			enqueue(std::move(line));
		}
	}

	std::string take_session()
	{
		std::lock_guard lock(session_mutex);
		subscriptions.clear();
		return std::exchange(session_id, std::string{});
	}

	// The session is over without stop(): logs `why` (if any), closes the session, calls on_eof. Once.
	void end_session(const char* why)
	{
		std::lock_guard lock(gate);
		if (stopping || ended) {
			return;
		}
		ended = true;
		if (why != nullptr) {
			application.log(log_level::warn, why);
		}
		if (std::string closed = take_session(); !closed.empty()) {
			application.session_closed(std::move(closed));
		}
		if (config.on_eof) {
			config.on_eof();
		}
	}

	void mark_done(bool& flag)
	{
		{
			std::lock_guard lock(threads_mutex);
			flag = true;
		}
		threads_cv.notify_all();
	}

	void write_loop()
	{
		for (;;) {
			std::string line;
			{
				std::unique_lock lock(out_mutex);
				out_cv.wait(lock, [this] { return out_broken || out_closing || !queue.empty(); });
				if (out_broken || queue.empty()) {
					break;
				}
				line = std::move(queue.front());
				queue.pop_front();
				queued_bytes -= line.size();
				writing = true;
			}
			const bool written = !config.write || config.write(line);
			{
				std::lock_guard lock(out_mutex);
				writing = false;
				if (!written) {
					out_broken = true;
					queue.clear();
					queued_bytes = 0;
				}
			}
			out_cv.notify_all();
		}
		bool broken = false;
		{
			std::lock_guard lock(out_mutex);
			broken = out_broken;
		}
		if (broken) {
			end_session("the MCP stdio client does not take the output any more: the session ends");
		}
		mark_done(writer_done);
	}

	void read_loop()
	{
		std::vector<char> buffer(read_chunk);
		std::string pending;
		std::size_t scanned = 0;
		bool fatal = false;
		while (!fatal) {
			const std::size_t got = source->read(buffer.data(), buffer.size());
			if (got == 0) {
				break;
			}
			pending.append(buffer.data(), got);
			std::size_t begin = 0;
			for (;;) {
				const std::size_t newline = pending.find('\n', std::max(begin, scanned));
				if (newline == std::string::npos) {
					break;
				}
				if (newline - begin > config.max_line_bytes) {
					fatal = true;
					break;
				}
				handle_line(std::string_view(pending).substr(begin, newline - begin));
				begin = newline + 1;
			}
			if (!fatal) {
				pending.erase(0, begin);
				scanned = pending.size();
				fatal = pending.size() > config.max_line_bytes;
			}
		}
		if (fatal) {
			send(rpc_error(nullptr, invalid_request, "message too long"));
			end_session("a line on stdin is longer than the limit: the MCP session ends");
		} else {
			if (!pending.empty()) {
				// The last line may come without its '\n'.
				handle_line(pending);
			}
			end_session(nullptr);
		}
		mark_done(reader_done);
	}

	void handle_line(std::string_view line)
	{
		std::lock_guard lock(gate);
		// Once the session is over (EOF, a dead output) nobody would get the answers: the input is only discarded.
		if (stopping || ended || output_dead()) {
			return;
		}
		handle_line_locked(line);
	}

	bool output_dead()
	{
		std::lock_guard lock(out_mutex);
		return out_broken;
	}

	void handle_line_locked(std::string_view line)
	{
		if (!line.empty() && line.back() == '\r') {
			line.remove_suffix(1);
		}
		if (line.find_first_not_of(" \t") == std::string_view::npos) {
			return;
		}

		json::parse_options options;
		options.max_depth = 64;
		boost::system::error_code ec;
		json::value message = json::parse(line, ec, json::storage_ptr(), options);
		if (ec) {
			send(rpc_error(nullptr, parse_error, "parse error"));
			return;
		}
		if (message.is_array()) {
			send(rpc_error(nullptr, invalid_request, "batch requests are not supported"));
			return;
		}
		if (!message.is_object()) {
			send(rpc_error(nullptr, invalid_request, "a JSON-RPC message is an object"));
			return;
		}

		rpc_message rpc_in = classify_rpc(message.get_object());
		if (rpc_in.type == rpc_message::kind::response) {
			return;
		}
		if (rpc_in.type == rpc_message::kind::invalid) {
			send(rpc_error(nullptr, invalid_request, rpc_in.error));
			return;
		}
		const bool is_request = rpc_in.type == rpc_message::kind::request;
		if (rpc_in.bad_params) {
			if (is_request) {
				send(rpc_error(rpc_in.id, invalid_params, "params must be an object"));
			}
			return;
		}

		if (rpc_in.method == "initialize" && is_request) {
			bool accepted = false;
			const json::value answer = rpc.initialize(rpc_in.id, rpc_in.params, accepted);
			std::string line_out = json::serialize(answer);
			line_out.push_back('\n');
			std::string replaced;
			{
				// The result is queued before the new id is published: nothing of the new session can precede it.
				std::lock_guard session_lock(session_mutex);
				enqueue(std::move(line_out));
				if (accepted) {
					replaced = std::exchange(session_id, new_session_id());
					subscriptions.clear();
				}
			}
			if (!replaced.empty()) {
				application.session_closed(std::move(replaced));
			}
			return;
		}

		std::string current;
		{
			std::lock_guard session_lock(session_mutex);
			current = session_id;
		}
		if (current.empty()) {
			if (is_request) {
				send(rpc_error(rpc_in.id, invalid_request, "no session: send initialize first"));
			}
			return;
		}
		std::weak_ptr<core> weak = weak_from_this();
		rpc_session session;
		session.id = current;
		if (!is_request) {
			// notifications/initialized, notifications/cancelled and the rest need no answer.
			try {
				rpc.notification(rpc_in.method, rpc_in.params, session);
			} catch (const std::exception& e) {
				application.log(log_level::error, std::string("notification failed: ") + e.what());
			} catch (...) {
				application.log(log_level::error, "notification failed");
			}
			return;
		}
		session.notify = [weak, current](const json::value& note) {
			if (const auto self = weak.lock()) {
				self->send_to(current, note);
			}
		};
		session.subscribe = [weak, current](const std::string& uri, bool subscribe) {
			const auto self = weak.lock();
			if (!self) {
				return true;
			}
			std::lock_guard session_lock(self->session_mutex);
			if (self->session_id != current) {
				return true;
			}
			if (!subscribe) {
				self->subscriptions.erase(uri);
				return true;
			}
			if (self->subscriptions.size() >= self->config.max_subscriptions && self->subscriptions.count(uri) == 0) {
				return false;
			}
			self->subscriptions.insert(uri);
			return true;
		};
		auto answered = std::make_shared<std::atomic<bool>>(false);
		const rpc_dispatcher::reply_sink reply = [weak, current, answered](json::value answer) {
			if (answered->exchange(true)) {
				return;
			}
			if (const auto self = weak.lock()) {
				self->send_to(current, answer);
			}
		};
		try {
			rpc.dispatch(rpc_in.method, rpc_in.id, rpc_in.params, session, reply);
		} catch (const std::exception& e) {
			application.log(log_level::error, std::string("request failed: ") + e.what());
			reply(rpc_error(rpc_in.id, internal_error, "internal error"));
		} catch (...) {
			reply(rpc_error(rpc_in.id, internal_error, "internal error"));
		}
	}

	// Waits up to `timeout` for `flag`, calling `poke` every 50 ms meanwhile.
	bool wait_done(const bool& flag, std::chrono::milliseconds timeout, const std::function<void()>& poke)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		std::unique_lock lock(threads_mutex);
		while (!flag) {
			if (std::chrono::steady_clock::now() >= deadline) {
				return false;
			}
			lock.unlock();
			poke();
			lock.lock();
			threads_cv.wait_for(lock, std::chrono::milliseconds(50), [&flag] { return flag; });
		}
		return true;
	}
};

mcp::stdio_transport::stdio_transport(host& application, stdio_config config)
	: m_core(std::make_shared<core>(application, std::move(config)))
{
	m_core->writer = std::thread([self = m_core] { self->write_loop(); });
}

mcp::stdio_transport::~stdio_transport()
{
	stop();
}

void mcp::stdio_transport::start(std::unique_ptr<byte_source> source)
{
	if (!source || m_core->reader.joinable() || m_core->source) {
		return;
	}
	m_core->source = std::move(source);
	m_core->reader = std::thread([self = m_core] { self->read_loop(); });
}

void mcp::stdio_transport::stop()
{
	core& c = *m_core;
	{
		std::lock_guard lock(c.gate);
		if (c.stopping) {
			return;
		}
		c.stopping = true;
	}
	if (std::string closed = c.take_session(); !closed.empty()) {
		c.application.session_closed(std::move(closed));
	}

	if (c.reader.joinable()) {
		// Again and again: a Windows read that had not begun yet when it was first interrupted cannot be cancelled.
		if (c.wait_done(c.reader_done, c.config.stop_timeout, [&c] { c.source->interrupt(); })) {
			c.reader.join();
		} else {
			// A read that cannot be cancelled (a console on Windows): the thread keeps only the core alive and ends
			// whenever its read returns; it calls nothing outside the transport any more.
			c.application.log(log_level::warn, "the MCP stdin reader did not stop in time and is left behind");
			c.reader.detach();
		}
	}

	{
		std::lock_guard lock(c.out_mutex);
		c.out_closing = true;
	}
	c.out_cv.notify_all();
	if (c.writer.joinable()) {
		if (c.wait_done(c.writer_done, c.config.stop_timeout, [] {})) {
			c.writer.join();
		} else {
			c.application.log(log_level::warn, "the MCP stdout writer is blocked by a client that does not read and is left behind");
			c.writer.detach();
		}
	}
}

void mcp::stdio_transport::handle_line(std::string_view line)
{
	m_core->handle_line(line);
}

bool mcp::stdio_transport::flush(std::chrono::milliseconds timeout)
{
	core& c = *m_core;
	std::unique_lock lock(c.out_mutex);
	return c.out_cv.wait_for(lock, timeout, [&c] { return c.out_broken || (c.queue.empty() && !c.writing); });
}

std::string mcp::stdio_transport::session_id() const
{
	std::lock_guard lock(m_core->session_mutex);
	return m_core->session_id;
}

void mcp::stdio_transport::notify_tools_changed()
{
	core& c = *m_core;
	std::string line = json::serialize(rpc_notification("notifications/tools/list_changed", json::value(nullptr)));
	line.push_back('\n');
	std::lock_guard lock(c.session_mutex);
	if (!c.session_id.empty()) {
		c.enqueue(std::move(line));
	}
}

void mcp::stdio_transport::notify_resource_updated(const std::string& uri)
{
	core& c = *m_core;
	json::object params;
	params["uri"] = json::string(uri);
	std::string line = json::serialize(rpc_notification("notifications/resources/updated", std::move(params)));
	line.push_back('\n');
	std::lock_guard lock(c.session_mutex);
	if (!c.session_id.empty() && c.subscriptions.count(uri) != 0) {
		c.enqueue(std::move(line));
	}
}
