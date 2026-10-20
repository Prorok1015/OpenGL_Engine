// Windows: Asio needs the target version before any system header, and the system headers must not define min/max.
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <snk/mcp/mcp_server.h>

#include <snk/mcp/mcp_endpoint.h>
#include <snk/mcp/mcp_http.h>

#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/string.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace asio = boost::asio;
using tcp = boost::asio::ip::tcp;

namespace mcp::detail
{
	class connection;

	// How many frames of an event stream may wait to be written. A subscriber that does not read falls behind and is
	// dropped instead of growing the write queue.
	constexpr std::size_t max_stream_backlog = 256;

	// Everything the server shares with its connections. The connections hold it by shared_ptr, and it holds them
	// only weakly, so no cycle keeps it alive after stop().
	class server_core : public std::enable_shared_from_this<server_core>
	{
	public:
		server_core(host& application_, server_config config_);
		~server_core();

		bool start();
		void stop();
		bool running() const
		{
			return running_flag.load() && !thread_done.load();
		}

		host& application;
		server_config config;
		endpoint ep;
		asio::io_context io{ 1 };
		tcp::acceptor acceptor{ io };
		asio::steady_timer retry_timer{ io };
		asio::executor_work_guard<asio::io_context::executor_type> work{ asio::make_work_guard(io) };
		std::thread thread;
		std::atomic<bool> running_flag{ false };
		// Set when the I/O thread has left run(), however it got there.
		std::atomic<bool> thread_done{ false };
		std::atomic<std::uint16_t> bound_port{ 0 };
		std::atomic<std::size_t> open_connections{ 0 };
		// I/O thread only.
		std::vector<std::weak_ptr<connection>> connections;

	private:
		void accept_next();
		void run();
	};

	// One client socket: reads requests, hands them to the endpoint, writes the responses, and feeds an event stream.
	// All members are touched on the I/O thread only; answers that come from other threads are posted to it.
	// Every handler runs inside guarded(): an exception ends that connection, not the I/O loop.
	class connection : public std::enable_shared_from_this<connection>
	{
	public:
		connection(std::shared_ptr<server_core> core, tcp::socket socket)
			: m_core(std::move(core))
			, m_socket(std::move(socket))
			, m_timer(m_core->io)
			, m_write_timer(m_core->io)
			, m_parser(http_limits{ m_core->config.max_header_bytes, 64, m_core->config.max_message_bytes })
		{
			++m_core->open_connections;
		}

		~connection()
		{
			--m_core->open_connections;
		}

		void start()
		{
			guarded([this] { read_more(); });
		}

		void close()
		{
			if (m_closed) {
				return;
			}
			m_closed = true;
			m_awaiting_hold.reset();
			m_timer.cancel();
			m_write_timer.cancel();
			boost::system::error_code ignored;
			m_socket.shutdown(tcp::socket::shutdown_both, ignored);
			m_socket.close(ignored);
			if (m_channel) {
				m_channel->set_wake({});
				m_channel->close();
			}
		}

		// The answer of the host, posted to the I/O thread.
		void deliver_from_host(http_response response)
		{
			guarded([&] { deliver(std::move(response)); });
		}

		void flush_from_stream()
		{
			guarded([this] { flush_stream(); });
		}

	private:
		struct outgoing
		{
			std::string data;
			bool close_after = false;
		};

		template<class F>
		void guarded(F&& body)
		{
			try {
				body();
			} catch (const std::exception& e) {
				fail(e.what());
			} catch (...) {
				fail("unknown exception");
			}
		}

		void fail(const char* what)
		{
			m_core->application.log(log_level::error, std::string("an MCP connection failed and was closed: ") + what);
			close();
		}

		void arm_timer(std::chrono::milliseconds limit)
		{
			m_timer.expires_after(limit);
			auto self = shared_from_this();
			m_timer.async_wait([self](const boost::system::error_code& ec) {
				if (!ec) {
					self->close();
				}
			});
		}

		void read_more()
		{
			if (m_closed) {
				return;
			}
			// Waiting for the first byte of a request: the idle limit. Once a request has begun its one deadline
			// (armed in feed) keeps running; a byte arriving does not renew it.
			if (!m_in_request) {
				arm_timer(m_core->config.idle_timeout);
			}
			auto self = shared_from_this();
			m_socket.async_read_some(asio::buffer(m_chunk), [self](const boost::system::error_code& ec, std::size_t count) {
				self->guarded([&] {
					if (ec) {
						self->close();
						return;
					}
					self->feed(std::string_view(self->m_chunk.data(), count));
				});
			});
		}

		void feed(std::string_view data)
		{
			const http_request_parser::state outcome = m_parser.feed(data);
			if (!m_in_request && (!data.empty() || m_parser.buffered())) {
				m_in_request = true;
				arm_timer(m_core->config.request_timeout);
			}
			switch (outcome) {
			case http_request_parser::state::need_more:
				if (m_parser.expects_continue() && !m_sent_continue) {
					m_sent_continue = true;
					// "100 Continue" invites the client to send the body; not for a request that would be refused.
					if (std::optional<http_response> denied = m_core->ep.check_access(m_parser.pending_request())) {
						m_awaiting = true;
						deliver(std::move(*denied));
						return;
					}
					write("HTTP/1.1 100 Continue\r\n\r\n", false);
				}
				read_more();
				return;
			case http_request_parser::state::error:
				reject(m_parser.error_status(), m_parser.error_message());
				return;
			case http_request_parser::state::complete:
				dispatch(m_parser.take());
				return;
			}
		}

		void reject(int status, const std::string& message)
		{
			boost::json::object body;
			body["error"] = boost::json::string(std::string_view(message));
			http_response response;
			response.status = status;
			response.close = true;
			response.set_header("Content-Type", "application/json");
			response.body = boost::json::serialize(body);
			m_awaiting = true;
			deliver(std::move(response));
		}

		void dispatch(http_request request)
		{
			// The host may take as long as it needs; only the wait for the client is timed.
			m_timer.cancel();
			m_in_request = false;
			m_awaiting = true;
			m_keep_alive = request.keep_alive;
			m_sent_continue = false;
			// While the host works no operation is pending on the socket, and the server holds connections weakly:
			// without this the connection would be gone before the answer is posted back. Released in deliver()/close().
			m_awaiting_hold = shared_from_this();
			std::weak_ptr<connection> weak = weak_from_this();
			std::shared_ptr<server_core> core = m_core;
			m_core->ep.handle(request, [core, weak](http_response response) {
				asio::post(core->io, [weak, response = std::move(response)]() mutable {
					if (auto self = weak.lock()) {
						self->deliver_from_host(std::move(response));
					}
				});
			});
		}

		void deliver(http_response response)
		{
			if (m_closed || !m_awaiting) {
				return;
			}
			m_awaiting = false;
			// The caller (the posted answer, or a read handler) holds the connection, so this cannot destroy it.
			m_awaiting_hold.reset();
			if (response.stream) {
				start_stream(std::move(response));
				return;
			}
			const bool close_after = response.close || !m_keep_alive;
			write(serialize_response(response), close_after);
			if (!close_after) {
				if (m_out_bytes > m_core->config.max_pending_output_bytes) {
					// The client does not read its answers: no new request until it has. on_written() resumes.
					m_read_paused = true;
					return;
				}
				// A pipelined request may already be buffered; otherwise this reads.
				feed(std::string_view());
			}
		}

		void start_stream(http_response response)
		{
			m_channel = std::move(response.stream);
			m_timer.cancel();
			write(serialize_stream_head(response), false);
			std::weak_ptr<connection> weak = weak_from_this();
			std::weak_ptr<server_core> weak_core = m_core;
			m_channel->set_wake([weak, weak_core] {
				const std::shared_ptr<server_core> core = weak_core.lock();
				if (!core) {
					return;
				}
				asio::post(core->io, [weak] {
					if (auto self = weak.lock()) {
						self->flush_from_stream();
					}
				});
			});
			watch_peer();
		}

		void flush_stream()
		{
			if (m_closed || !m_channel) {
				return;
			}
			std::vector<std::string> frames = m_channel->take();
			const bool closed = m_channel->closed();
			std::size_t new_bytes = 0;
			for (const std::string& frame : frames) {
				new_bytes += frame.size();
			}
			if (m_out.size() + frames.size() > max_stream_backlog || m_out_bytes + new_bytes > m_core->config.max_stream_backlog_bytes) {
				m_core->application.log(log_level::warn, "an MCP event stream does not keep up and was closed");
				close();
				return;
			}
			for (std::string& frame : frames) {
				write(std::move(frame), false);
			}
			if (closed) {
				m_close_when_idle = true;
				maybe_close();
			}
		}

		// Reads and drops whatever the client sends on an event stream; the end of the connection is the news.
		void watch_peer()
		{
			auto self = shared_from_this();
			m_socket.async_read_some(asio::buffer(m_chunk), [self](const boost::system::error_code& ec, std::size_t) {
				self->guarded([&] {
					if (ec) {
						self->close();
						return;
					}
					self->watch_peer();
				});
			});
		}

		void write(std::string data, bool close_after)
		{
			if (m_closed) {
				return;
			}
			m_out_bytes += data.size();
			m_out.push_back({ std::move(data), close_after });
			if (!m_writing) {
				write_next();
			}
		}

		void write_next()
		{
			if (m_out.empty()) {
				m_writing = false;
				maybe_close();
				return;
			}
			m_writing = true;
			auto self = shared_from_this();
			// The sequence number tells a deadline that was already queued when the write finished from the one of the
			// write in flight.
			const std::uint64_t write_id = ++m_write_seq;
			m_write_timer.expires_after(m_core->config.write_timeout);
			m_write_timer.async_wait([self, write_id](const boost::system::error_code& ec) {
				if (!ec && self->m_writing && self->m_write_seq == write_id) {
					self->m_core->application.log(log_level::warn, "an MCP client does not read its answers and was closed");
					self->close();
				}
			});
			asio::async_write(m_socket, asio::buffer(m_out.front().data), [self](const boost::system::error_code& ec, std::size_t) {
				self->guarded([&] { self->on_written(ec); });
			});
		}

		void on_written(const boost::system::error_code& ec)
		{
			m_write_timer.cancel();
			if (ec || m_closed) {
				close();
				return;
			}
			const bool close_after = m_out.front().close_after;
			m_out_bytes -= m_out.front().data.size();
			m_out.pop_front();
			if (close_after) {
				linger();
				return;
			}
			write_next();
			if (m_read_paused && m_out_bytes <= m_core->config.max_pending_output_bytes / 2) {
				m_read_paused = false;
				feed(std::string_view());
			}
		}

		// Ends the connection without a reset: closing a socket that still has unread input (the rest of a refused
		// request) sends RST, and the client could lose the response. So the send side is shut down first, the input
		// is read and dropped until the client closes, and a timer ends it if the client never does.
		void linger()
		{
			if (m_closed) {
				return;
			}
			boost::system::error_code ignored;
			m_socket.shutdown(tcp::socket::shutdown_send, ignored);
			arm_timer(std::chrono::seconds(5));
			drain_input();
		}

		void drain_input()
		{
			auto self = shared_from_this();
			m_socket.async_read_some(asio::buffer(m_chunk), [self](const boost::system::error_code& ec, std::size_t) {
				self->guarded([&] {
					if (ec) {
						self->close();
						return;
					}
					self->drain_input();
				});
			});
		}

		void maybe_close()
		{
			if (m_close_when_idle && !m_writing && m_out.empty()) {
				close();
			}
		}

		std::shared_ptr<server_core> m_core;
		tcp::socket m_socket;
		asio::steady_timer m_timer;
		asio::steady_timer m_write_timer;
		std::uint64_t m_write_seq = 0;
		http_request_parser m_parser;
		std::array<char, 8192> m_chunk{};
		std::deque<outgoing> m_out;
		std::shared_ptr<event_channel> m_channel;
		std::shared_ptr<connection> m_awaiting_hold;
		bool m_closed = false;
		bool m_awaiting = false;
		bool m_in_request = false;
		bool m_keep_alive = true;
		bool m_sent_continue = false;
		bool m_writing = false;
		bool m_close_when_idle = false;
		// Bytes in m_out, and whether reading is held back because they pile up.
		std::size_t m_out_bytes = 0;
		bool m_read_paused = false;
	};

	namespace {
		endpoint_config make_endpoint_config(const server_config& config)
		{
			endpoint_config out;
			out.token = config.token;
			out.allowed_origins = config.allowed_origins;
			out.max_sessions = config.max_sessions;
			out.info = config.info;
			return out;
		}
	}

	server_core::server_core(host& application_, server_config config_)
		: application(application_)
		, config(std::move(config_))
		, ep(application_, make_endpoint_config(config))
	{
	}

	server_core::~server_core()
	{
		stop();
	}

	bool server_core::start()
	{
		if (running_flag.load() || thread.joinable()) {
			return false;
		}
		if (config.token.empty()) {
			application.log(log_level::error, "the MCP server needs a token, it was not started");
			return false;
		}
		boost::system::error_code ec;
		const tcp::endpoint local(asio::ip::address_v4::loopback(), config.port);
		acceptor.open(local.protocol(), ec);
		if (!ec) {
#ifdef _WIN32
			// SO_REUSEADDR on Windows would let another process bind the same port; the exclusive option forbids it.
			int exclusive = 1;
			if (::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
				reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
				application.log(log_level::warn, "the MCP server could not take its port exclusively");
			}
#else
			// Lets a restarted editor take its port back at once.
			boost::system::error_code option_ec;
			acceptor.set_option(tcp::acceptor::reuse_address(true), option_ec);
#endif
			acceptor.bind(local, ec);
		}
		if (!ec) {
			acceptor.listen(asio::socket_base::max_listen_connections, ec);
		}
		if (!ec) {
			const tcp::endpoint bound = acceptor.local_endpoint(ec);
			if (!ec) {
				bound_port = bound.port();
			}
		}
		if (ec) {
			application.log(log_level::error, "the MCP server cannot listen on 127.0.0.1:" + std::to_string(config.port) + ": " + ec.message());
			boost::system::error_code ignored;
			acceptor.close(ignored);
			return false;
		}
		ep.set_port(bound_port.load());
		accept_next();
		running_flag = true;
		thread = std::thread([this] { run(); });
		application.log(log_level::info, "the MCP server listens on http://127.0.0.1:" + std::to_string(bound_port.load()) + "/mcp");
		return true;
	}

	void server_core::run()
	{
		// A handler that throws ends run(), not the server: log it and go on, unless the context was stopped.
		for (;;) {
			try {
				io.run();
				break;
			} catch (const std::exception& e) {
				application.log(log_level::error, std::string("the MCP server I/O loop caught an exception: ") + e.what());
			} catch (...) {
				application.log(log_level::error, "the MCP server I/O loop caught an exception");
			}
			if (io.stopped()) {
				break;
			}
		}
		thread_done = true;
	}

	void server_core::stop()
	{
		if (!running_flag.exchange(false)) {
			return;
		}
		// On the I/O thread, which owns the connections. Once the sockets are closed and the work guard is gone,
		// run() returns after the cancelled operations have released their connections.
		// Each step on its own: a throw in one must not skip work.reset(), or join() below would wait for ever.
		asio::post(io, [this] {
			boost::system::error_code ignored;
			acceptor.close(ignored);
			try {
				retry_timer.cancel();
			} catch (...) {
			}
			try {
				ep.close_all();
			} catch (...) {
			}
			for (const std::weak_ptr<connection>& weak : connections) {
				try {
					if (const std::shared_ptr<connection> alive = weak.lock()) {
						alive->close();
					}
				} catch (...) {
				}
			}
			connections.clear();
			work.reset();
		});
		if (thread.joinable()) {
			thread.join();
		}
	}

	void server_core::accept_next()
	{
		acceptor.async_accept([this](const boost::system::error_code& ec, tcp::socket socket) {
			if (ec == asio::error::operation_aborted) {
				return;
			}
			try {
				if (ec) {
					application.log(log_level::warn, "the MCP server could not accept a connection: " + ec.message());
					// A persistent failure (out of descriptors) must not spin.
					retry_timer.expires_after(std::chrono::milliseconds(100));
					retry_timer.async_wait([this](const boost::system::error_code& wait_ec) {
						if (!wait_ec && acceptor.is_open()) {
							accept_next();
						}
					});
					return;
				}
				if (open_connections.load() >= config.max_connections) {
					boost::system::error_code ignored;
					socket.close(ignored);
				} else {
					std::erase_if(connections, [](const std::weak_ptr<connection>& weak) { return weak.expired(); });
					auto client = std::make_shared<connection>(shared_from_this(), std::move(socket));
					connections.push_back(client);
					client->start();
				}
			} catch (const std::exception& e) {
				application.log(log_level::error, std::string("the MCP server failed to take a connection: ") + e.what());
			} catch (...) {
				application.log(log_level::error, "the MCP server failed to take a connection");
			}
			if (acceptor.is_open()) {
				accept_next();
			}
		});
	}
}

mcp::server::server(host& application, server_config config)
	: m_core(std::make_shared<detail::server_core>(application, std::move(config)))
{
}

mcp::server::~server()
{
	m_core->stop();
}

bool mcp::server::start()
{
	return m_core->start();
}

void mcp::server::stop()
{
	m_core->stop();
}

bool mcp::server::running() const
{
	return m_core->running();
}

std::uint16_t mcp::server::port() const
{
	return m_core->bound_port.load();
}

void mcp::server::notify_resource_updated(const std::string& uri)
{
	m_core->ep.notify_resource_updated(uri);
}

void mcp::server::notify_tools_changed()
{
	m_core->ep.notify_tools_changed();
}
