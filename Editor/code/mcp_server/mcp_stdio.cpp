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
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
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
			const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
			if (input == nullptr || input == INVALID_HANDLE_VALUE) {
				return 0;
			}
			DWORD got = 0;
			const DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(size, read_chunk));
			// ERROR_OPERATION_ABORTED (interrupt), ERROR_BROKEN_PIPE (the writer is gone): the end of the input.
			if (!ReadFile(input, buffer, wanted, &got, nullptr)) {
				return 0;
			}
			return static_cast<std::size_t>(got);
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

struct mcp::stdio_transport::output
{
	std::mutex mutex;
	std::function<void(std::string_view)> write;

	void send(const json::value& message)
	{
		std::string line = json::serialize(message);
		line.push_back('\n');
		std::lock_guard lock(mutex);
		if (write) {
			write(line);
		}
	}
};

struct mcp::stdio_transport::session_state
{
	mutable std::mutex mutex;
	std::string id;
	std::set<std::string> subscriptions;
};

mcp::stdio_transport::stdio_transport(host& application, stdio_config config)
	: m_host(application)
	, m_config(std::move(config))
	, m_rpc(application, m_config.info)
	, m_output(std::make_shared<output>())
	, m_session(std::make_shared<session_state>())
{
	m_output->write = m_config.write;
}

mcp::stdio_transport::~stdio_transport()
{
	stop();
}

void mcp::stdio_transport::start(std::unique_ptr<byte_source> source)
{
	if (!source || m_reader.joinable()) {
		return;
	}
	m_source = std::move(source);
	m_reader = std::thread([this] { read_loop(); });
}

void mcp::stdio_transport::stop()
{
	{
		std::lock_guard lock(m_reader_mutex);
		m_stopping = true;
	}
	if (m_reader.joinable()) {
		std::unique_lock lock(m_reader_mutex);
		// Again and again: a Windows read that had not begun yet when it was first interrupted cannot be cancelled.
		while (!m_reader_done) {
			lock.unlock();
			m_source->interrupt();
			lock.lock();
			m_reader_cv.wait_for(lock, std::chrono::milliseconds(50), [this] { return m_reader_done; });
		}
		lock.unlock();
		m_reader.join();
	}
	std::string closed;
	{
		std::lock_guard lock(m_session->mutex);
		closed.swap(m_session->id);
		m_session->subscriptions.clear();
	}
	if (!closed.empty()) {
		m_host.session_closed(std::move(closed));
	}
}

std::string mcp::stdio_transport::session_id() const
{
	std::lock_guard lock(m_session->mutex);
	return m_session->id;
}

void mcp::stdio_transport::send(const json::value& message)
{
	m_output->send(message);
}

void mcp::stdio_transport::read_loop()
{
	std::vector<char> buffer(read_chunk);
	std::string pending;
	std::size_t scanned = 0;
	bool fatal = false;
	while (!fatal) {
		const std::size_t got = m_source->read(buffer.data(), buffer.size());
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
			if (newline - begin > m_config.max_line_bytes) {
				fatal = true;
				break;
			}
			handle_line(std::string_view(pending).substr(begin, newline - begin));
			begin = newline + 1;
		}
		if (!fatal) {
			pending.erase(0, begin);
			scanned = pending.size();
			fatal = pending.size() > m_config.max_line_bytes;
		}
	}
	if (fatal) {
		send(rpc_error(nullptr, invalid_request, "message too long"));
	} else if (!pending.empty()) {
		// The last line may come without its '\n'.
		handle_line(pending);
	}
	end_of_input();
	{
		std::lock_guard lock(m_reader_mutex);
		m_reader_done = true;
	}
	m_reader_cv.notify_all();
}

void mcp::stdio_transport::end_of_input()
{
	bool stopping = false;
	{
		std::lock_guard lock(m_reader_mutex);
		if (m_eof_handled) {
			return;
		}
		m_eof_handled = true;
		stopping = m_stopping;
	}
	std::string closed;
	{
		std::lock_guard lock(m_session->mutex);
		closed.swap(m_session->id);
		m_session->subscriptions.clear();
	}
	if (!closed.empty()) {
		m_host.session_closed(std::move(closed));
	}
	if (!stopping && m_config.on_eof) {
		m_config.on_eof();
	}
}

void mcp::stdio_transport::handle_line(std::string_view line)
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

	rpc_message rpc = classify_rpc(message.get_object());
	if (rpc.type == rpc_message::kind::response) {
		return;
	}
	if (rpc.type == rpc_message::kind::invalid) {
		send(rpc_error(nullptr, invalid_request, rpc.error));
		return;
	}
	const bool is_request = rpc.type == rpc_message::kind::request;
	if (rpc.bad_params) {
		if (is_request) {
			send(rpc_error(rpc.id, invalid_params, "params must be an object"));
		}
		return;
	}

	if (rpc.method == "initialize" && is_request) {
		bool accepted = false;
		json::value answer = m_rpc.initialize(rpc.id, rpc.params, accepted);
		if (accepted) {
			// A second initialize starts over: the session it replaces is closed like at the end of the input.
			std::string replaced;
			{
				std::lock_guard lock(m_session->mutex);
				replaced = std::exchange(m_session->id, new_session_id());
				m_session->subscriptions.clear();
			}
			if (!replaced.empty()) {
				m_host.session_closed(std::move(replaced));
			}
		}
		send(answer);
		return;
	}

	const std::string current = session_id();
	if (current.empty()) {
		if (is_request) {
			send(rpc_error(rpc.id, invalid_request, "no session: send initialize first"));
		}
		return;
	}
	if (!is_request) {
		// notifications/initialized, notifications/cancelled and the rest need no answer.
		return;
	}

	rpc_session session;
	session.id = current;
	session.notify = [out = m_output](const json::value& note) { out->send(note); };
	session.subscribe = [state = m_session, limit = m_config.max_subscriptions, current](const std::string& uri, bool subscribe) {
		std::lock_guard lock(state->mutex);
		if (state->id != current) {
			return true;
		}
		if (!subscribe) {
			state->subscriptions.erase(uri);
			return true;
		}
		if (state->subscriptions.size() >= limit && state->subscriptions.count(uri) == 0) {
			return false;
		}
		state->subscriptions.insert(uri);
		return true;
	};
	auto answered = std::make_shared<std::atomic<bool>>(false);
	const rpc_dispatcher::reply_sink reply = [out = m_output, answered](json::value answer) {
		if (!answered->exchange(true)) {
			out->send(answer);
		}
	};
	try {
		m_rpc.dispatch(rpc.method, rpc.id, rpc.params, session, reply);
	} catch (const std::exception& e) {
		m_host.log(log_level::error, std::string("request failed: ") + e.what());
		reply(rpc_error(rpc.id, internal_error, "internal error"));
	} catch (...) {
		reply(rpc_error(rpc.id, internal_error, "internal error"));
	}
}

void mcp::stdio_transport::notify_tools_changed()
{
	if (session_id().empty()) {
		return;
	}
	send(rpc_notification("notifications/tools/list_changed", json::value(nullptr)));
}

void mcp::stdio_transport::notify_resource_updated(const std::string& uri)
{
	{
		std::lock_guard lock(m_session->mutex);
		if (m_session->id.empty() || m_session->subscriptions.count(uri) == 0) {
			return;
		}
	}
	json::object params;
	params["uri"] = json::string(uri);
	send(rpc_notification("notifications/resources/updated", std::move(params)));
}
