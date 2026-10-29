#include <mcp/mcp_server.h>
#include <mcp/mcp_endpoint.h>
#include "mcp_util.h"
// Asio first: on Windows it pulls in winsock2.h before anything can include windows.h.
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace json = boost::json;
using tcp = net::ip::tcp;

namespace mcp::transport
{
	class connection;

	// Everything one running server owns. The io thread is the only one that touches `connections`.
	struct core
	{
		core(host& served_, server_config cfg_)
			: cfg(std::move(cfg_))
			, served(served_)
			, io(1)
			, ep(served_, cfg)
			, acceptor(io)
		{
		}

		server_config cfg;
		host& served;
		net::io_context io;
		endpoint ep;
		tcp::acceptor acceptor;
		std::unordered_set<std::shared_ptr<connection>> connections;
		bool closing = false;
	};

	http_request describe(const http::request_header<http::fields>& head)
	{
		http_request out;
		out.method = std::string(head.method_string());
		out.target = std::string(head.target());
		for (const auto& field : head) out.headers.emplace_back(std::string(field.name_string()), std::string(field.value()));
		return out;
	}

	// One client connection: reads a request (head first, so a refused client's body is never read), hands it to the
	// endpoint, writes the answer. All members run on the io thread.
	class connection : public std::enable_shared_from_this<connection>
	{
	public:
		connection(core& owner, std::weak_ptr<core> weak_owner, tcp::socket socket)
			: m_core(owner)
			, m_weak_core(std::move(weak_owner))
			, m_stream(std::move(socket))
		{
		}

		void start() { read_head(); }

		void close()
		{
			if (m_closed) return;
			m_closed = true;
			const auto self = shared_from_this();
			beast::error_code ignored;
			m_stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
			m_stream.close();
			m_core.connections.erase(self);
		}

		// The endpoint's answer.
		void send(http_response response)
		{
			if (m_closed) return;
			if (response.stream) {
				begin_stream(response);
				return;
			}
			const bool keep = m_keep_alive && !response.close;
			auto reply = std::make_shared<http::response<http::string_body>>(static_cast<http::status>(response.status), 11u);
			for (const auto& [name, value] : response.headers) reply->set(name, value);
			reply->set(http::field::server, "snk-mcp");
			reply->body() = std::move(response.body);
			reply->keep_alive(keep);
			reply->prepare_payload();
			m_stream.expires_after(m_core.cfg.io_timeout);
			http::async_write(m_stream, *reply, [self = shared_from_this(), reply, keep](beast::error_code ec, std::size_t) {
				if (ec) {
					self->close();
					return;
				}
				if (!keep) {
					self->close_after_response();
					return;
				}
				self->read_head();
			});
		}

		// An event for the open stream.
		void enqueue(std::string chunk)
		{
			if (m_closed || m_finishing) return;
			m_out.push_back(std::move(chunk));
			pump();
		}

		// No more events: send what is queued, then close.
		void finish()
		{
			if (m_closed) return;
			m_finishing = true;
			pump();
		}

	private:
		// The answer is written and the connection ends. A refused client may still be sending its body: closing at once
		// with that unread would reset the connection and the client could lose the answer. So the write side is shut
		// down first (the client sees the end of the answer) and what the client still sends is read and dropped, up to a
		// limit, until it closes or the timeout runs out.
		void close_after_response()
		{
			beast::error_code ignored;
			m_stream.socket().shutdown(tcp::socket::shutdown_send, ignored);
			drain();
		}

		void drain()
		{
			if (m_closed) return;
			if (m_drained > drain_limit) {
				close();
				return;
			}
			m_stream.expires_after(m_core.cfg.io_timeout);
			m_stream.async_read_some(net::buffer(m_scratch), [self = shared_from_this()](beast::error_code ec, std::size_t got) {
				if (ec) {
					self->close();
					return;
				}
				self->m_drained += got;
				self->drain();
			});
		}

		void read_head()
		{
			if (m_closed) return;
			m_parser.emplace();
			m_parser->body_limit(static_cast<std::uint64_t>(m_core.cfg.max_message_bytes));
			m_parser->header_limit(16 * 1024);
			m_stream.expires_after(m_core.cfg.io_timeout);
			http::async_read_header(m_stream, m_buffer, *m_parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
				self->on_head(ec);
			});
		}

		void on_head(beast::error_code ec)
		{
			if (ec) {
				close();
				return;
			}
			const auto& head = m_parser->get();
			m_keep_alive = head.keep_alive();
			const auto declared = m_parser->content_length();
			const std::uint64_t length = declared ? *declared : 0;
			if (auto refused = m_core.ep.screen(describe(head), length)) {
				send(std::move(*refused));
				return;
			}
			if (beast::iequals(head[http::field::expect], "100-continue")) {
				auto text = std::make_shared<std::string>("HTTP/1.1 100 Continue\r\n\r\n");
				net::async_write(m_stream, net::buffer(*text), [self = shared_from_this(), text](beast::error_code written, std::size_t) {
					if (written) {
						self->close();
						return;
					}
					self->read_body();
				});
				return;
			}
			read_body();
		}

		void read_body()
		{
			m_stream.expires_after(m_core.cfg.io_timeout);
			http::async_read(m_stream, m_buffer, *m_parser, [self = shared_from_this()](beast::error_code ec, std::size_t) {
				self->on_body(ec);
			});
		}

		void on_body(beast::error_code ec);

		void begin_stream(const http_response& response)
		{
			std::string text = "HTTP/1.1 " + std::to_string(response.status) + " "
				+ std::string(http::obsolete_reason(static_cast<http::status>(response.status))) + "\r\n";
			for (const auto& [name, value] : response.headers) text += name + ": " + value + "\r\n";
			text += "Connection: close\r\n\r\n";

			m_keep_alive = false;
			m_stream.expires_never();
			m_writing = true;
			auto head = std::make_shared<std::string>(std::move(text));
			net::async_write(m_stream, net::buffer(*head), [self = shared_from_this(), head](beast::error_code ec, std::size_t) {
				self->m_writing = false;
				if (ec) {
					self->close();
					return;
				}
				self->m_started = true;
				self->pump();
			});
			// A client that goes away ends the stream; nothing else is read from it.
			m_stream.socket().async_wait(tcp::socket::wait_read, [self = shared_from_this()](beast::error_code) { self->close(); });
		}

		void pump()
		{
			if (m_closed || !m_started || m_writing) return;
			if (m_out.empty()) {
				if (m_finishing) close();
				return;
			}
			auto text = std::make_shared<std::string>(std::move(m_out.front()));
			m_out.pop_front();
			m_writing = true;
			net::async_write(m_stream, net::buffer(*text), [self = shared_from_this(), text](beast::error_code ec, std::size_t) {
				self->m_writing = false;
				if (ec) {
					self->close();
					return;
				}
				self->pump();
			});
		}

		core& m_core;
		std::weak_ptr<core> m_weak_core;
		beast::tcp_stream m_stream;
		beast::flat_buffer m_buffer;
		std::optional<http::request_parser<http::string_body>> m_parser;
		bool m_keep_alive = false;
		bool m_closed = false;
		// close_after_response(): what is read and dropped.
		static constexpr std::size_t drain_limit = 1024 * 1024;
		std::size_t m_drained = 0;
		std::array<char, 4096> m_scratch{};
		// Event stream state.
		std::deque<std::string> m_out;
		bool m_started = false;
		bool m_writing = false;
		bool m_finishing = false;
	};

	// Writes from any thread; the data goes through the io thread.
	class connection_sink final : public stream_sink
	{
	public:
		connection_sink(std::weak_ptr<core> owner, std::weak_ptr<connection> target)
			: m_owner(std::move(owner))
			, m_target(std::move(target))
		{
		}

		void write(std::string_view chunk) override
		{
			const auto owner = m_owner.lock();
			if (!owner) return;
			net::post(owner->io, [target = m_target, text = std::string(chunk)]() mutable {
				if (const auto live = target.lock()) live->enqueue(std::move(text));
			});
		}

		void close() override
		{
			const auto owner = m_owner.lock();
			if (!owner) return;
			net::post(owner->io, [target = m_target]() {
				if (const auto live = target.lock()) live->finish();
			});
		}

	private:
		std::weak_ptr<core> m_owner;
		std::weak_ptr<connection> m_target;
	};

	void connection::on_body(beast::error_code ec)
	{
		if (ec == http::error::body_limit) {
			http_response refused;
			refused.status = 413;
			refused.close = true;
			refused.set("Content-Type", "application/json");
			refused.body = "{\"error\":\"the message is too large\"}";
			send(std::move(refused));
			return;
		}
		if (ec) {
			close();
			return;
		}
		m_keep_alive = m_parser->get().keep_alive();
		auto message = m_parser->release();
		http_request request = describe(message);
		request.body = std::move(message.body());

		auto sink = std::make_shared<connection_sink>(m_weak_core, weak_from_this());
		reply_fn reply = [weak_core = m_weak_core, weak_self = weak_from_this()](http_response response) {
			const auto owner = weak_core.lock();
			if (!owner) return;
			net::post(owner->io, [weak_self, response = std::move(response)]() mutable {
				if (const auto live = weak_self.lock()) live->send(std::move(response));
			});
		};
		m_core.ep.handle(std::move(request), std::move(reply), std::move(sink));
	}

	void accept_next(core& owner, const std::weak_ptr<core>& weak_owner)
	{
		owner.acceptor.async_accept([&owner, weak_owner](beast::error_code ec, tcp::socket socket) {
			if (owner.closing || ec == net::error::operation_aborted) return;
			if (!ec) {
				if (owner.connections.size() >= owner.cfg.max_connections) {
					beast::error_code ignored;
					socket.close(ignored);
				} else {
					auto accepted = std::make_shared<connection>(owner, weak_owner, std::move(socket));
					owner.connections.insert(accepted);
					accepted->start();
				}
			}
			accept_next(owner, weak_owner);
		});
	}

	bool write_connection_file(const std::filesystem::path& file, const std::string& text, std::string& error)
	{
		std::error_code ec;
		if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
#ifdef _WIN32
		{
			std::ofstream out(file, std::ios::binary | std::ios::trunc);
			if (!out) {
				error = "cannot create " + file.string();
				return false;
			}
			out << text;
			out.flush();
			if (!out) {
				error = "cannot write " + file.string();
				return false;
			}
		}
		// Windows keeps a user's profile private through its ACLs; this only drops the read-only bit.
		std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
		return true;
#else
		const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd < 0) {
			error = "cannot create " + file.string() + ": " + std::error_code(errno, std::generic_category()).message();
			return false;
		}
		// An existing file keeps its old mode through O_CREAT: fix it before the token goes in.
		if (::fchmod(fd, 0600) != 0) {
			error = "cannot restrict " + file.string() + ": " + std::error_code(errno, std::generic_category()).message();
			::close(fd);
			return false;
		}
		std::size_t done = 0;
		while (done < text.size()) {
			const ssize_t wrote = ::write(fd, text.data() + done, text.size() - done);
			if (wrote < 0) {
				if (errno == EINTR) continue;
				error = "cannot write " + file.string() + ": " + std::error_code(errno, std::generic_category()).message();
				::close(fd);
				return false;
			}
			done += static_cast<std::size_t>(wrote);
		}
		::close(fd);
		return true;
#endif
	}
}

struct mcp::server::impl
{
	impl(host& served_, server_config cfg_)
		: served(served_)
		, cfg(std::move(cfg_))
	{
	}

	host& served;
	const server_config cfg;

	mutable std::mutex mutex;
	std::shared_ptr<transport::core> owner;
	std::thread thread;
	std::string token;
	std::uint16_t port = 0;
	std::string error;
	std::filesystem::path written_file;
};

mcp::server::server(host& served, server_config cfg)
	: m_impl(std::make_unique<impl>(served, std::move(cfg)))
{
}

mcp::server::~server()
{
	stop();
}

bool mcp::server::start()
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->owner) return true;
	m_impl->error.clear();

	const auto fail = [this](std::string message) {
		m_impl->error = message;
		m_impl->served.log(level::error, "MCP server: " + message);
		return false;
	};

	server_config cfg = m_impl->cfg;
	if (cfg.token.empty()) cfg.token = util::random_hex(32);

	std::filesystem::path written;
	try {
		auto owner = std::make_shared<transport::core>(m_impl->served, cfg);
		beast::error_code ec;
		const tcp::endpoint where(net::ip::address_v4::loopback(), cfg.port);
		owner->acceptor.open(where.protocol(), ec);
		if (ec) return fail("cannot open the listener: " + ec.message());
		owner->acceptor.bind(where, ec);
		if (ec) return fail("cannot bind 127.0.0.1:" + std::to_string(cfg.port) + ": " + ec.message());
		owner->acceptor.listen(net::socket_base::max_listen_connections, ec);
		if (ec) return fail("cannot listen: " + ec.message());
		const tcp::endpoint bound = owner->acceptor.local_endpoint(ec);
		if (ec) return fail("cannot read the bound port: " + ec.message());

		const std::uint16_t port = bound.port();
		owner->ep.set_port(port);
		const std::string url = "http://127.0.0.1:" + std::to_string(port) + cfg.path;

		if (!cfg.connection_file.empty()) {
			json::object info;
			info["url"] = util::js(url);
			info["port"] = static_cast<std::int64_t>(port);
			info["token"] = util::js(cfg.token);
			std::string file_error;
			if (!transport::write_connection_file(cfg.connection_file, json::serialize(info) + "\n", file_error)) return fail(file_error);
			written = cfg.connection_file;
		}

		transport::accept_next(*owner, owner);
		m_impl->thread = std::thread([raw = owner.get()] { raw->io.run(); });
		m_impl->token = cfg.token;
		m_impl->port = port;
		m_impl->written_file = std::move(written);
		m_impl->owner = std::move(owner);
		m_impl->served.log(level::info, "MCP server listening on " + url);
		return true;
	} catch (const std::exception& e) {
		if (!written.empty()) {
			std::error_code ignored;
			std::filesystem::remove(written, ignored);
		}
		return fail(std::string("cannot start: ") + e.what());
	}
}

void mcp::server::stop()
{
	std::shared_ptr<transport::core> owner;
	std::thread thread;
	std::filesystem::path file;
	{
		std::lock_guard lock(m_impl->mutex);
		owner = std::move(m_impl->owner);
		thread = std::move(m_impl->thread);
		file = std::move(m_impl->written_file);
		m_impl->owner.reset();
		m_impl->written_file.clear();
	}
	if (!owner) return;

	net::post(owner->io, [raw = owner.get()] {
		raw->closing = true;
		beast::error_code ignored;
		raw->acceptor.close(ignored);
		const auto open = raw->connections;
		for (const auto& live : open) live->close();
		raw->ep.close_all();
	});
	if (thread.joinable()) thread.join();
	if (!file.empty()) {
		std::error_code ignored;
		std::filesystem::remove(file, ignored);
	}
	m_impl->served.log(level::info, "MCP server stopped");
}

bool mcp::server::running() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->owner != nullptr;
}

std::uint16_t mcp::server::port() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->port;
}

std::string mcp::server::token() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->token;
}

std::string mcp::server::url() const
{
	std::lock_guard lock(m_impl->mutex);
	return "http://127.0.0.1:" + std::to_string(m_impl->port) + m_impl->cfg.path;
}

std::string mcp::server::error() const
{
	std::lock_guard lock(m_impl->mutex);
	return m_impl->error;
}

void mcp::server::notify_resource_updated(const std::string& uri)
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->owner) m_impl->owner->ep.notify_resource_updated(uri);
}

void mcp::server::notify_tools_changed()
{
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->owner) m_impl->owner->ep.notify_tools_changed();
}
