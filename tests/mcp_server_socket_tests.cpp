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

#include <boost/test/unit_test.hpp>
#include "mcp_test_support.hpp"
#include <snk/mcp/mcp_server.h>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/parse.hpp>
#include <boost/system/error_code.hpp>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// snk_mcp_server over real sockets on 127.0.0.1, port chosen by the operating system, with the fake host. The test
// thread is a blocking client; nothing sleeps: every wait is a read that returns when the server writes (a watchdog
// closes the socket after 20 s so a broken server fails the test instead of hanging it).

namespace {

namespace asio = boost::asio;
namespace json = boost::json;
using tcp = asio::ip::tcp;
using mcp_test::fake_host;
using mcp_test::json_of;

struct client_response
{
	int status = 0;
	std::map<std::string, std::string> headers; // lower-case names
	std::string body;
};

class test_client
{
public:
	explicit test_client(std::uint16_t port)
		: m_socket(m_io)
	{
		m_socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port));
		m_watchdog = std::thread([this] {
			std::unique_lock lock(m_watch_mutex);
			if (!m_watch_cv.wait_for(lock, std::chrono::seconds(20), [this] { return m_done; })) {
				boost::system::error_code ignored;
				m_socket.close(ignored);
			}
		});
	}

	~test_client()
	{
		{
			std::lock_guard lock(m_watch_mutex);
			m_done = true;
		}
		m_watch_cv.notify_all();
		m_watchdog.join();
	}

	test_client(const test_client&) = delete;
	test_client& operator=(const test_client&) = delete;

	void send(const std::string& text)
	{
		asio::write(m_socket, asio::buffer(text));
	}

	// One response with a Content-Length body; nullopt when the server closed the connection first.
	std::optional<client_response> read_response()
	{
		boost::system::error_code ec;
		const std::size_t head_size = asio::read_until(m_socket, asio::dynamic_buffer(m_buffer), "\r\n\r\n", ec);
		if (ec) {
			return std::nullopt;
		}
		const std::string head = m_buffer.substr(0, head_size);
		m_buffer.erase(0, head_size);

		client_response response;
		response.status = std::stoi(head.substr(9, 3));
		std::size_t line_begin = head.find("\r\n") + 2;
		while (line_begin < head.size()) {
			const std::size_t line_end = head.find("\r\n", line_begin);
			if (line_end == line_begin || line_end == std::string::npos) {
				break;
			}
			const std::string line = head.substr(line_begin, line_end - line_begin);
			const std::size_t colon = line.find(':');
			if (colon != std::string::npos) {
				std::string name = line.substr(0, colon);
				for (char& c : name) {
					c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				std::size_t value_begin = colon + 1;
				while (value_begin < line.size() && line[value_begin] == ' ') {
					++value_begin;
				}
				response.headers[name] = line.substr(value_begin);
			}
			line_begin = line_end + 2;
		}

		const auto length = response.headers.find("content-length");
		if (length != response.headers.end()) {
			const std::size_t wanted = static_cast<std::size_t>(std::stoul(length->second));
			if (m_buffer.size() < wanted) {
				asio::read(m_socket, asio::dynamic_buffer(m_buffer), asio::transfer_exactly(wanted - m_buffer.size()), ec);
				if (ec) {
					return std::nullopt;
				}
			}
			response.body = m_buffer.substr(0, wanted);
			m_buffer.erase(0, wanted);
		}
		return response;
	}

	// Reads (an event stream) until the text contains `needle`; the text so far, empty if the server closed first.
	std::string read_until_text(const std::string& needle)
	{
		boost::system::error_code ec;
		asio::read_until(m_socket, asio::dynamic_buffer(m_buffer), needle, ec);
		if (ec) {
			return {};
		}
		return m_buffer;
	}

	// True when the server closes the connection (nothing more to read).
	bool reaches_end_of_stream()
	{
		boost::system::error_code ec;
		char byte = 0;
		while (!ec) {
			m_socket.read_some(asio::buffer(&byte, 1), ec);
		}
		return ec == asio::error::eof || ec == asio::error::connection_reset;
	}

private:
	asio::io_context m_io;
	tcp::socket m_socket;
	std::string m_buffer;
	std::mutex m_watch_mutex;
	std::condition_variable m_watch_cv;
	bool m_done = false;
	std::thread m_watchdog;
};

std::string request_text(std::uint16_t port, const std::string& method, const std::string& body, const std::string& session = {},
	const std::string& token = mcp_test::test_token, const std::string& extra_headers = {})
{
	std::string text = method + " /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\n";
	if (!token.empty()) {
		text += "Authorization: Bearer " + token + "\r\n";
	}
	if (!session.empty()) {
		text += "Mcp-Session-Id: " + session + "\r\n";
	}
	text += extra_headers;
	if (method == "POST") {
		text += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
	}
	text += "\r\n" + body;
	return text;
}

std::string rpc(int id, const std::string& method, const std::string& params = "{}")
{
	return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + method + R"(","params":)" + params + "}";
}

mcp::server_config make_config()
{
	mcp::server_config config;
	config.token = mcp_test::test_token;
	config.info.name = "socket-test";
	return config;
}

std::string session_of(const client_response& response)
{
	const auto found = response.headers.find("mcp-session-id");
	return found == response.headers.end() ? std::string() : found->second;
}

constexpr const char* initialize_params = R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}})";

}

BOOST_AUTO_TEST_SUITE(McpServerSocketTests)

BOOST_AUTO_TEST_CASE(Start_BindsALoopbackPortAndStopsCleanly)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_TEST(!server.running());
	BOOST_TEST(server.port() == 0);
	BOOST_REQUIRE(server.start());
	BOOST_TEST(server.running());
	BOOST_TEST(server.port() != 0);
	BOOST_TEST(!server.start());
	server.stop();
	BOOST_TEST(!server.running());
	server.stop();
}

BOOST_AUTO_TEST_CASE(Start_WithoutATokenFails)
{
	fake_host host;
	mcp::server_config config = make_config();
	config.token.clear();
	mcp::server server(host, config);
	BOOST_TEST(!server.start());
	BOOST_TEST(!server.running());
	BOOST_TEST(!host.logs().empty());
}

BOOST_AUTO_TEST_CASE(Start_OnAPortInUseFails)
{
	fake_host first_host;
	mcp::server first(first_host, make_config());
	BOOST_REQUIRE(first.start());

	fake_host second_host;
	mcp::server_config config = make_config();
	config.port = first.port();
	mcp::server second(second_host, config);
	BOOST_TEST(!second.start());
	BOOST_TEST(!second_host.logs().empty());
}

BOOST_AUTO_TEST_CASE(OneConnection_InitializeListAndCallATool)
{
	fake_host host;
	host.set_tools({ mcp_test::make_tool("log_read") });
	mcp::call_result answer;
	answer.structured = json_of(R"({"entries":[]})");
	host.set_next_result(answer);
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client client(port);
	client.send(request_text(port, "POST", rpc(1, "initialize", initialize_params)));
	const auto init = client.read_response();
	BOOST_REQUIRE(init.has_value());
	BOOST_REQUIRE_EQUAL(init->status, 200);
	BOOST_TEST(init->headers.at("content-type") == "application/json");
	const std::string session = session_of(*init);
	BOOST_REQUIRE(!session.empty());
	BOOST_TEST(json_of(init->body).at("result").at("serverInfo").at("name").as_string().size() == std::string("socket-test").size());

	client.send(request_text(port, "POST", R"({"jsonrpc":"2.0","method":"notifications/initialized"})", session));
	const auto accepted = client.read_response();
	BOOST_REQUIRE(accepted.has_value());
	BOOST_TEST(accepted->status == 202);

	client.send(request_text(port, "POST", rpc(2, "tools/list"), session));
	const auto listed = client.read_response();
	BOOST_REQUIRE(listed.has_value());
	BOOST_REQUIRE_EQUAL(listed->status, 200);
	BOOST_TEST(json_of(listed->body).at("result").at("tools").as_array().size() == 1u);

	client.send(request_text(port, "POST", rpc(3, "tools/call", R"({"name":"log_read","arguments":{"n":1}})"), session));
	const auto called = client.read_response();
	BOOST_REQUIRE(called.has_value());
	BOOST_REQUIRE_EQUAL(called->status, 200);
	BOOST_TEST(json_of(called->body).at("result").at("structuredContent").at("entries").as_array().empty());
	const auto calls = host.calls();
	BOOST_REQUIRE_EQUAL(calls.size(), 1u);
	BOOST_TEST(calls[0].session_id == session);
	BOOST_TEST(calls[0].name == "log_read");
}

BOOST_AUTO_TEST_CASE(PipelinedRequests_AreAnsweredInOrder)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client client(port);
	client.send(request_text(port, "POST", rpc(10, "initialize", initialize_params)));
	const auto init = client.read_response();
	BOOST_REQUIRE(init.has_value());
	const std::string session = session_of(*init);

	client.send(request_text(port, "POST", rpc(11, "ping"), session) + request_text(port, "POST", rpc(12, "ping"), session));
	const auto first = client.read_response();
	const auto second = client.read_response();
	BOOST_REQUIRE(first.has_value());
	BOOST_REQUIRE(second.has_value());
	BOOST_TEST(json_of(first->body).at("id").as_int64() == 11);
	BOOST_TEST(json_of(second->body).at("id").as_int64() == 12);
}

BOOST_AUTO_TEST_CASE(NoToken_Is401AndTheServerHangsUp)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client client(port);
	client.send(request_text(port, "POST", rpc(1, "initialize", initialize_params), {}, {}));
	const auto response = client.read_response();
	BOOST_REQUIRE(response.has_value());
	BOOST_TEST(response->status == 401);
	BOOST_TEST(response->headers.at("www-authenticate").rfind("Bearer", 0) == 0u);
	BOOST_TEST(client.reaches_end_of_stream());
	BOOST_TEST(host.calls().empty());
}

BOOST_AUTO_TEST_CASE(WrongHostAndOrigin_Are403)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	{
		test_client client(port);
		std::string text = request_text(port, "POST", rpc(1, "ping"));
		text.replace(text.find("127.0.0.1:"), 10, "evil.test:");
		client.send(text);
		const auto response = client.read_response();
		BOOST_REQUIRE(response.has_value());
		BOOST_TEST(response->status == 403);
		BOOST_TEST(client.reaches_end_of_stream());
	}
	{
		test_client client(port);
		client.send(request_text(port, "POST", rpc(1, "ping"), {}, mcp_test::test_token, "Origin: http://evil.test\r\n"));
		const auto response = client.read_response();
		BOOST_REQUIRE(response.has_value());
		BOOST_TEST(response->status == 403);
	}
}

BOOST_AUTO_TEST_CASE(ExpectContinue_IsAnsweredOnlyForARequestThatPassesTheChecks)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();
	const std::string body = rpc(1, "initialize", initialize_params);

	{
		// Allowed: "100 Continue", then the body is sent and the real answer comes.
		test_client client(port);
		std::string head = request_text(port, "POST", body, {}, mcp_test::test_token, "Expect: 100-continue\r\n");
		head.resize(head.size() - body.size());
		client.send(head);
		const auto interim = client.read_response();
		BOOST_REQUIRE(interim.has_value());
		BOOST_TEST(interim->status == 100);
		client.send(body);
		const auto final_response = client.read_response();
		BOOST_REQUIRE(final_response.has_value());
		BOOST_TEST(final_response->status == 200);
	}
	{
		// No token: refused straight away, the invitation to send the body is never made.
		test_client client(port);
		std::string head = request_text(port, "POST", body, {}, {}, "Expect: 100-continue\r\n");
		head.resize(head.size() - body.size());
		client.send(head);
		const auto response = client.read_response();
		BOOST_REQUIRE(response.has_value());
		BOOST_TEST(response->status == 401);
	}
}

BOOST_AUTO_TEST_CASE(RequestDeadline_CutsOffAClientThatNeverFinishesTheHead)
{
	fake_host host;
	mcp::server_config config = make_config();
	config.request_timeout = std::chrono::milliseconds(100);
	mcp::server server(host, config);
	BOOST_REQUIRE(server.start());

	test_client client(server.port());
	client.send("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n");
	BOOST_TEST(client.reaches_end_of_stream());
}

BOOST_AUTO_TEST_CASE(IdleDeadline_ClosesAConnectionThatSendsNothing)
{
	fake_host host;
	mcp::server_config config = make_config();
	config.idle_timeout = std::chrono::milliseconds(100);
	mcp::server server(host, config);
	BOOST_REQUIRE(server.start());

	test_client client(server.port());
	BOOST_TEST(client.reaches_end_of_stream());
}

BOOST_AUTO_TEST_CASE(OversizedBody_Is413)
{
	fake_host host;
	mcp::server_config config = make_config();
	config.max_message_bytes = 64;
	mcp::server server(host, config);
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client client(port);
	client.send(request_text(port, "POST", std::string(200, ' ')));
	const auto response = client.read_response();
	BOOST_REQUIRE(response.has_value());
	BOOST_TEST(response->status == 413);
	BOOST_TEST(client.reaches_end_of_stream());
}

BOOST_AUTO_TEST_CASE(Garbage_Is400)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());

	test_client client(server.port());
	client.send("this is not http\r\n\r\n");
	const auto response = client.read_response();
	BOOST_REQUIRE(response.has_value());
	BOOST_TEST(response->status == 400);
}

BOOST_AUTO_TEST_CASE(EventStream_DeliversNotificationsAndEndsOnStop)
{
	fake_host host;
	mcp::resource_info log_resource;
	log_resource.uri = "snk://log";
	log_resource.name = "Log";
	host.set_resources({ log_resource });
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client rpc_client(port);
	rpc_client.send(request_text(port, "POST", rpc(1, "initialize", initialize_params)));
	const auto init = rpc_client.read_response();
	BOOST_REQUIRE(init.has_value());
	const std::string session = session_of(*init);

	test_client stream(port);
	stream.send(request_text(port, "GET", {}, session, mcp_test::test_token, "Accept: text/event-stream\r\n"));
	// The first frame says the stream is attached; after it a notification cannot be missed.
	const std::string opened = stream.read_until_text(": stream open\n\n");
	BOOST_REQUIRE(!opened.empty());
	BOOST_TEST(opened.find("200 OK") != std::string::npos);
	BOOST_TEST(opened.find("text/event-stream") != std::string::npos);

	rpc_client.send(request_text(port, "POST", rpc(2, "resources/subscribe", R"({"uri":"snk://log"})"), session));
	const auto subscribed = rpc_client.read_response();
	BOOST_REQUIRE(subscribed.has_value());
	BOOST_TEST(subscribed->status == 200);

	server.notify_resource_updated("snk://log");
	const std::string updated = stream.read_until_text("notifications/resources/updated");
	BOOST_TEST(!updated.empty());
	BOOST_TEST(updated.find("snk://log") != std::string::npos);

	server.notify_tools_changed();
	BOOST_TEST(!stream.read_until_text("notifications/tools/list_changed").empty());

	server.stop();
	BOOST_TEST(stream.reaches_end_of_stream());
	BOOST_TEST(rpc_client.reaches_end_of_stream());
}

BOOST_AUTO_TEST_CASE(DeleteSession_ClosesItsStream)
{
	fake_host host;
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client rpc_client(port);
	rpc_client.send(request_text(port, "POST", rpc(1, "initialize", initialize_params)));
	const auto session_init = rpc_client.read_response();
	BOOST_REQUIRE(session_init.has_value());
	const std::string session = session_of(*session_init);

	test_client stream(port);
	stream.send(request_text(port, "GET", {}, session, mcp_test::test_token, "Accept: text/event-stream\r\n"));
	BOOST_REQUIRE(!stream.read_until_text(": stream open\n\n").empty());

	rpc_client.send(request_text(port, "DELETE", {}, session));
	const auto deleted = rpc_client.read_response();
	BOOST_REQUIRE(deleted.has_value());
	BOOST_TEST(deleted->status == 200);
	BOOST_TEST(stream.reaches_end_of_stream());
}

BOOST_AUTO_TEST_CASE(SlowTool_AnswersFromAnotherThreadWhileTheServerStaysResponsive)
{
	fake_host host;
	host.set_defer_calls(true);
	mcp::server server(host, make_config());
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client slow(port);
	slow.send(request_text(port, "POST", rpc(1, "initialize", initialize_params)));
	const auto slow_init = slow.read_response();
	BOOST_REQUIRE(slow_init.has_value());
	const std::string session = session_of(*slow_init);
	slow.send(request_text(port, "POST", rpc(2, "tools/call", R"({"name":"slow"})"), session));
	BOOST_REQUIRE(host.wait_for_deferred(1));

	test_client other(port);
	other.send(request_text(port, "POST", rpc(3, "initialize", initialize_params)));
	const auto other_init = other.read_response();
	BOOST_REQUIRE(other_init.has_value());
	BOOST_TEST(other_init->status == 200);

	mcp::call_result answer;
	answer.content.push_back(mcp::content_block::make_text("finally"));
	std::thread answering([&host, answer] { host.answer_deferred(answer); });
	const auto response = slow.read_response();
	answering.join();
	BOOST_REQUIRE(response.has_value());
	BOOST_REQUIRE_EQUAL(response->status, 200);
	BOOST_TEST(json_of(response->body).at("id").as_int64() == 2);
	BOOST_TEST(json_of(response->body).at("result").at("content").as_array().size() == 1u);
}

BOOST_AUTO_TEST_CASE(ConnectionLimit_DropsTheExtraConnection)
{
	fake_host host;
	mcp::server_config config = make_config();
	config.max_connections = 1;
	mcp::server server(host, config);
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	test_client first(port);
	first.send(request_text(port, "POST", rpc(1, "initialize", initialize_params)));
	BOOST_REQUIRE(first.read_response().has_value());

	test_client second(port);
	BOOST_TEST(second.reaches_end_of_stream());
}

BOOST_AUTO_TEST_CASE(ManyClients_AtOnce)
{
	fake_host host;
	host.set_tools({ mcp_test::make_tool("t") });
	mcp::server_config config = make_config();
	config.max_sessions = 16;
	mcp::server server(host, config);
	BOOST_REQUIRE(server.start());
	const std::uint16_t port = server.port();

	std::atomic<int> good{ 0 };
	std::vector<std::thread> clients;
	for (int c = 0; c < 4; ++c) {
		clients.emplace_back([&, c] {
			test_client client(port);
			client.send(request_text(port, "POST", rpc(100 + c, "initialize", initialize_params)));
			const auto init = client.read_response();
			if (!init || init->status != 200) {
				return;
			}
			const std::string session = session_of(*init);
			for (int i = 0; i < 5; ++i) {
				client.send(request_text(port, "POST", rpc(i, "tools/list"), session));
				const auto listed = client.read_response();
				if (listed && listed->status == 200) {
					++good;
				}
			}
		});
	}
	for (std::thread& thread : clients) {
		thread.join();
	}
	BOOST_TEST(good.load() == 20);
}

BOOST_AUTO_TEST_SUITE_END()
