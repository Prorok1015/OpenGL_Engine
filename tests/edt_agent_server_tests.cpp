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
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_access.h"
#include "edt_agent_server.h"
#include "edt_agent_server_settings.h"
#include <snk/command/cmd_registry.h>
#include <snk/mcp/mcp_connection_file.h>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The person's switch for the agent server (#449): the settings file, and the HTTP transport on and off while the editor
// runs, with real sockets on 127.0.0.1.
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
namespace fs = std::filesystem;

// One blocking MCP `initialize` over HTTP: the session id the server gave, empty when it refused. A watchdog closes the
// socket after 20 s so a broken server fails the test instead of hanging it.
std::string open_session(std::uint16_t port, const std::string& token, const std::string& client_name)
{
	asio::io_context io;
	tcp::socket socket(io);
	boost::system::error_code ec;
	socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
	if (ec) return {};
	std::mutex mutex;
	std::condition_variable cv;
	bool done = false;
	std::thread watchdog([&] {
		std::unique_lock lock(mutex);
		if (!cv.wait_for(lock, std::chrono::seconds(20), [&] { return done; })) {
			boost::system::error_code ignored;
			socket.close(ignored);
		}
	});
	const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":")"
		+ client_name + R"("}}})";
	const std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nAuthorization: Bearer " + token
		+ "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: " + std::to_string(body.size())
		+ "\r\n\r\n" + body;
	std::string session;
	asio::write(socket, asio::buffer(request), ec);
	std::string buffer;
	if (!ec) {
		const std::size_t head_size = asio::read_until(socket, asio::dynamic_buffer(buffer), "\r\n\r\n", ec);
		if (!ec) {
			const std::string head = buffer.substr(0, head_size);
			const std::string key = "Mcp-Session-Id: ";
			if (const std::size_t at = head.find(key); at != std::string::npos) {
				const std::size_t end = head.find("\r\n", at);
				session = head.substr(at + key.size(), end - at - key.size());
			}
		}
	}
	{
		std::lock_guard lock(mutex);
		done = true;
	}
	cv.notify_all();
	watchdog.join();
	return session;
}

struct server_fixture
{
	test_util::temp_dir dir{ "edt_agent_server" };
	ds::task_system tasks{ test_util::manual_tasks() };
	cmd::command_registry commands;
	std::vector<std::string> closed;
	edt::agent_access access{ commands, dir.path() / "project.json", dir.path() / "personal.json" };
	std::unique_ptr<edt::agent_server> server;

	server_fixture()
	{
		edt::agent_server_options options;
		options.http = false;
		options.project_dir = dir.path();
		options.on_session_closed = [this](const std::string& session) { closed.push_back(session); };
		server = std::make_unique<edt::agent_server>(commands, tasks, access, std::move(options));
	}

	~server_fixture()
	{
		server.reset();
	}

	std::optional<mcp::connection_info> file() const
	{
		return mcp::read_connection_file(dir.path() / ".snk");
	}
};

}

BOOST_AUTO_TEST_SUITE(EdtAgentServerSettings)

BOOST_AUTO_TEST_CASE(NoFileOrABadOne_IsOff)
{
	test_util::temp_dir dir("edt_agent_server_settings_missing");
	BOOST_TEST((edt::load_agent_server_settings(dir.path() / "none.json") == edt::agent_server_settings{}));
	dir.write("bad.json", "{ nope");
	BOOST_TEST((edt::load_agent_server_settings(dir.path() / "bad.json") == edt::agent_server_settings{}));
	dir.write("typo.json", R"({"enabled":true,"prot":1})");
	BOOST_TEST(!edt::load_agent_server_settings(dir.path() / "typo.json").enabled);
}

BOOST_AUTO_TEST_CASE(ARoundTrip_KeepsTheChoice)
{
	test_util::temp_dir dir("edt_agent_server_settings_roundtrip");
	const edt::agent_server_settings chosen{ true, 8123 };
	BOOST_TEST(!edt::save_agent_server_settings(dir.path() / "settings" / "agent_server.json", chosen).has_value());
	BOOST_TEST((edt::load_agent_server_settings(dir.path() / "settings" / "agent_server.json") == chosen));
}

BOOST_AUTO_TEST_CASE(WrongTypesAndPortsAreErrors)
{
	for (const char* text : { "[]", R"({"enabled":1})", R"({"port":"80"})", R"({"port":-1})", R"({"port":65536})", R"({"port":1.5})", R"({"x":1})" }) {
		BOOST_TEST_CONTEXT(text) { BOOST_TEST(!edt::parse_agent_server_settings(text).error.empty()); }
	}
	BOOST_TEST(edt::parse_agent_server_settings("{}").error.empty());
	BOOST_TEST(edt::parse_agent_server_settings(R"({"port":65535,"enabled":false})").error.empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAgentServerRuntime)

BOOST_FIXTURE_TEST_CASE(HttpStartsAndStopsWhileTheServerObjectStays, server_fixture)
{
	BOOST_TEST(!server->running());
	BOOST_TEST(!file().has_value());

	BOOST_REQUIRE(server->start_http(0));
	BOOST_TEST(server->running());
	BOOST_TEST(server->port() != 0);
	BOOST_REQUIRE(file().has_value());
	BOOST_TEST(file()->port == server->port());

	// Again on the same port: nothing restarts, the token stays.
	const std::string token = file()->token;
	BOOST_TEST(server->start_http(0));
	BOOST_TEST(file()->token == token);

	server->stop_http();
	BOOST_TEST(!server->running());
	BOOST_TEST(!file().has_value());
	BOOST_TEST(server->sessions().empty());

	BOOST_TEST(server->start_http(0));
	BOOST_REQUIRE(file().has_value());
	BOOST_TEST(file()->token != token);
}

BOOST_FIXTURE_TEST_CASE(ADifferentPort_IsARestartWithANewToken, server_fixture)
{
	BOOST_REQUIRE(server->start_http(0));
	const std::string token = file()->token;
	const std::uint16_t first = server->port();

	// A free port the first run does not hold: the one a socket gets and gives back.
	std::uint16_t other = 0;
	{
		asio::io_context io;
		tcp::acceptor probe(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
		other = probe.local_endpoint().port();
	}
	BOOST_REQUIRE(other != first);
	BOOST_REQUIRE(server->start_http(other));
	BOOST_TEST(server->port() == other);
	BOOST_REQUIRE(file().has_value());
	BOOST_TEST(file()->token != token);
	BOOST_TEST(file()->port == other);
}

BOOST_FIXTURE_TEST_CASE(StoppingHttp_EndsEveryAgentSessionAndReportsEachOne, server_fixture)
{
	BOOST_REQUIRE(server->start_http(0));
	const std::string first = open_session(server->port(), file()->token, "claude-code");
	const std::string second = open_session(server->port(), file()->token, "other");
	BOOST_REQUIRE(!first.empty());
	BOOST_REQUIRE(!second.empty());

	const std::vector<edt::agent_session_info> listed = server->sessions();
	BOOST_REQUIRE_EQUAL(listed.size(), 2u);
	BOOST_TEST(listed[0].client_name == "claude-code");
	BOOST_TEST(listed[1].client_name == "other");

	server->stop_http();
	tasks.drain_main(std::chrono::hours(1));
	BOOST_REQUIRE_EQUAL(closed.size(), 2u);
	BOOST_TEST(((closed[0] == first && closed[1] == second) || (closed[0] == second && closed[1] == first)));
	BOOST_TEST(server->sessions().empty());
}

BOOST_FIXTURE_TEST_CASE(ClosingOneSession_ReportsItAndLeavesTheOthers, server_fixture)
{
	BOOST_REQUIRE(server->start_http(0));
	const std::string keep = open_session(server->port(), file()->token, "keep");
	const std::string drop = open_session(server->port(), file()->token, "drop");
	BOOST_REQUIRE(!keep.empty() && !drop.empty());

	BOOST_TEST(server->close_session(drop));
	tasks.drain_main(std::chrono::hours(1));
	BOOST_TEST(closed == std::vector<std::string>{ drop });
	const std::vector<edt::agent_session_info> listed = server->sessions();
	BOOST_REQUIRE_EQUAL(listed.size(), 1u);
	BOOST_TEST(listed[0].id == keep);
	BOOST_TEST(!server->close_session(drop));
}

BOOST_FIXTURE_TEST_CASE(TheEditorGoingDown_ReportsNoSessionEnd, server_fixture)
{
	BOOST_REQUIRE(server->start_http(0));
	BOOST_REQUIRE(!open_session(server->port(), file()->token, "client").empty());
	server.reset();
	tasks.drain_main(std::chrono::hours(1));
	BOOST_TEST(closed.empty());
	BOOST_TEST(!mcp::read_connection_file(dir.path() / ".snk").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
