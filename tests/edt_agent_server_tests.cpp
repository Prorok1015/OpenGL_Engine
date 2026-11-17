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
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/mcp/mcp_connection_file.h>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/system/error_code.hpp>
#include <array>
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

// One HTTP request sent on a thread of its own and read until the server closes the connection: everything the server
// sent. A watchdog closes the socket after `limit`, so a server that never lets go fails the test instead of hanging it.
class background_post
{
public:
	background_post(std::uint16_t port, std::string request, std::chrono::seconds limit)
		: m_socket(m_io)
	{
		m_reader = std::thread([this, port, request = std::move(request)] { run(port, request); });
		m_watchdog = std::thread([this, limit] {
			std::unique_lock lock(m_mutex);
			if (!m_cv.wait_for(lock, limit, [this] { return m_finished; })) {
				m_timed_out = true;
				boost::system::error_code ignored;
				m_socket.close(ignored);
			}
		});
	}

	~background_post()
	{
		m_reader.join();
		m_watchdog.join();
	}

	background_post(const background_post&) = delete;
	background_post& operator=(const background_post&) = delete;

	bool finished() const
	{
		std::lock_guard lock(m_mutex);
		return m_finished;
	}

	bool timed_out() const
	{
		std::lock_guard lock(m_mutex);
		return m_timed_out;
	}

	std::string received() const
	{
		std::lock_guard lock(m_mutex);
		return m_received;
	}

private:
	void run(std::uint16_t port, const std::string& request)
	{
		std::string received;
		boost::system::error_code ec;
		m_socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), port), ec);
		if (!ec) {
			asio::write(m_socket, asio::buffer(request), ec);
		}
		std::array<char, 4096> chunk{};
		while (!ec) {
			const std::size_t got = m_socket.read_some(asio::buffer(chunk), ec);
			received.append(chunk.data(), got);
		}
		{
			std::lock_guard lock(m_mutex);
			m_received = std::move(received);
			m_finished = true;
		}
		m_cv.notify_all();
	}

	asio::io_context m_io;
	tcp::socket m_socket;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_finished = false;
	bool m_timed_out = false;
	std::string m_received;
	std::thread m_reader;
	std::thread m_watchdog;
};

// The server with a job service: a tool call with a progressToken to a `starts_job` command is held until its job ends
// (#500). The test's job is queued on the manual task system and never runs before the test pumps the lanes.
struct held_job_fixture : server_fixture
{
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	ds::lifetime job_owner{ ds::lifetime_option::cancel_on_destroy };
	std::vector<cmd::registration> registrations;
	edt::job_service::job_id last_job = 0;

	held_job_fixture()
	{
		cmd::command_desc desc;
		desc.id = "test.held_job";
		desc.category = cmd::agent_category::read;
		desc.summary = "Starts a job that runs until it is cancelled";
		desc.starts_job = true;
		desc.result_schema = boost::json::parse(
			R"json({"type":"object","properties":{"job":{"type":"integer","minimum":0}},"required":["job"],"additionalProperties":false})json");
		desc.invoke = [this](const cmd::call&) {
			last_job = jobs.start_with_result(ds::lane::io, ds::task_desc{ "test held job", "test", true }, job_owner,
				[](ds::task_context& context) -> boost::json::value {
					context.throw_if_stopped();
					return boost::json::object{};
				});
			return cmd::result::success(boost::json::object{ { "job", static_cast<std::int64_t>(last_job) } });
		};
		cmd::registration registration = commands.add(std::move(desc));
		BOOST_REQUIRE(registration.active());
		registrations.push_back(std::move(registration));

		server.reset();
		edt::agent_server_options options;
		options.http = false;
		options.project_dir = dir.path();
		options.on_session_closed = [this](const std::string& session) { closed.push_back(session); };
		options.jobs = &jobs;
		options.events = &events;
		server = std::make_unique<edt::agent_server>(commands, tasks, access, std::move(options));
	}

	~held_job_fixture()
	{
		// The server and its bridge go before the job service and the events they were given.
		server.reset();
		test_util::pump(tasks);
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

BOOST_AUTO_TEST_CASE(TheCfgSwitch_HasThreeStates)
{
	using edt::cfg_switch;
	BOOST_TEST((edt::parse_cfg_switch("") == cfg_switch::unset));
	BOOST_TEST((edt::parse_cfg_switch("true") == cfg_switch::on));
	BOOST_TEST((edt::parse_cfg_switch("1") == cfg_switch::on));
	BOOST_TEST((edt::parse_cfg_switch("false") == cfg_switch::off));
	BOOST_TEST((edt::parse_cfg_switch("0") == cfg_switch::off));
	BOOST_TEST((edt::parse_cfg_switch("yes please") == cfg_switch::off));
}

BOOST_AUTO_TEST_CASE(TheCfgAndThePersonsFile_DecideTogether)
{
	using edt::cfg_switch;
	const edt::agent_server_settings on{ true, 8123 };
	const edt::agent_server_settings off{ false, 8123 };
	// cfg x file x headless x stdio -> enabled, port.
	struct row { cfg_switch cfg; edt::agent_server_settings file; bool headless; bool stdio; bool enabled; std::uint16_t port; };
	const row table[] = {
		// A person's session: cfg unset follows the file.
		{ cfg_switch::unset, on, false, false, true, 8123 },
		{ cfg_switch::unset, off, false, false, false, 8123 },
		{ cfg_switch::unset, {}, false, false, false, 0 },
		// cfg true wins, with its own port, whatever the file says.
		{ cfg_switch::on, off, false, false, true, 5000 },
		{ cfg_switch::on, on, false, false, true, 5000 },
		// An explicit cfg false wins over a file that says on.
		{ cfg_switch::off, on, false, false, false, 0 },
		{ cfg_switch::off, on, true, true, false, 0 },
		// A driven editor ignores the person's file.
		{ cfg_switch::unset, on, true, false, false, 0 },
		{ cfg_switch::unset, on, false, true, false, 0 },
		{ cfg_switch::unset, on, true, true, false, 0 },
		// ... but cfg true still opens it, headless or not.
		{ cfg_switch::on, off, true, true, true, 5000 },
	};
	for (const row& r : table) {
		BOOST_TEST_CONTEXT("cfg " << static_cast<int>(r.cfg) << " file " << r.file.enabled << " headless " << r.headless << " stdio " << r.stdio) {
			const edt::agent_http_choice got = edt::decide_agent_http(r.cfg, 5000, r.file, r.headless, r.stdio);
			BOOST_TEST(got.enabled == r.enabled);
			BOOST_TEST(got.port == r.port);
		}
	}
	// A cfg port that is not a port is "any".
	BOOST_TEST(edt::decide_agent_http(cfg_switch::on, 70000, {}, false, false).port == 0);
	BOOST_TEST(edt::decide_agent_http(cfg_switch::on, -1, {}, false, false).port == 0);
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

BOOST_FIXTURE_TEST_CASE(AFreePort_IsARestartWithANewToken_AndTheOldTokenIsRefused, server_fixture)
{
	// Two ports the operating system handed out, both free again (a port found this way can in theory be taken by another
	// process before the server binds it; then the test says so and stops).
	std::uint16_t first = 0;
	std::uint16_t second = 0;
	{
		asio::io_context io;
		tcp::acceptor one(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
		tcp::acceptor two(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0));
		first = one.local_endpoint().port();
		second = two.local_endpoint().port();
	}
	if (!server->start_http(first) || !server->start_http(second)) {
		BOOST_TEST_MESSAGE("a port was taken in between; the test cannot say anything");
		return;
	}
	BOOST_TEST(server->port() == second);
	// The first run's token: gone from the file, and the new server does not accept it.
	BOOST_REQUIRE(file().has_value());
	const std::string fresh = file()->token;
	BOOST_TEST(file()->port == second);
	BOOST_TEST(open_session(second, fresh, "new").size() == 32u);

	server->stop_http();
	BOOST_REQUIRE(server->start_http(0));
	BOOST_REQUIRE(file().has_value());
	BOOST_TEST(file()->token != fresh);
	BOOST_TEST(open_session(server->port(), fresh, "stale").empty());
	BOOST_TEST(open_session(server->port(), file()->token, "good").size() == 32u);
}

BOOST_FIXTURE_TEST_CASE(ATakenPort_LeavesTheRunningServerAsItWas, server_fixture)
{
	BOOST_REQUIRE(server->start_http(0));
	const std::uint16_t running_port = server->port();
	const std::string token = file()->token;
	const std::string kept = open_session(running_port, token, "kept");
	BOOST_REQUIRE(!kept.empty());

	asio::io_context io;
	tcp::acceptor holder(io, tcp::endpoint(asio::ip::address_v4::loopback(), 0), false);
	const std::uint16_t taken = holder.local_endpoint().port();

	BOOST_TEST(!server->start_http(taken));
	BOOST_TEST(server->running());
	BOOST_TEST(server->port() == running_port);
	BOOST_REQUIRE(file().has_value());
	BOOST_TEST(file()->token == token);
	BOOST_TEST(file()->port == running_port);
	// The session is still there and the token still opens a new one.
	BOOST_TEST(server->sessions().size() == 1u);
	BOOST_TEST(open_session(running_port, token, "again").size() == 32u);
	tasks.drain_main(std::chrono::hours(1));
	BOOST_TEST(closed.empty());
}

BOOST_FIXTURE_TEST_CASE(TheServerIsActiveOnlyWhileSomethingCanConnect, server_fixture)
{
	BOOST_TEST(!server->active());
	BOOST_REQUIRE(server->start_http(0));
	BOOST_TEST(server->active());
	server->stop_http();
	BOOST_TEST(!server->active());
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

// #500 with #508: a call held for its job when the person turns the HTTP transport off. The session's end cancels the
// job and drops the answer; the POST is let go (an empty 202, or the connection closed with the server) instead of
// hanging, the end is reported once, and nothing is left for tick() or for the server's destruction.
BOOST_FIXTURE_TEST_CASE(StoppingHttp_CancelsAHeldJobCall_AndLetsItsPostGo, held_job_fixture)
{
	const auto limit = std::chrono::seconds(10);
	BOOST_REQUIRE(server->start_http(0));
	const std::uint16_t port = server->port();
	BOOST_REQUIRE(file().has_value());
	const std::string token = file()->token;
	const std::string session = open_session(port, token, "held");
	BOOST_REQUIRE(!session.empty());

	const std::string body
		= R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"test_held_job","arguments":{},"_meta":{"progressToken":"p"}}})";
	const std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) + "\r\nAuthorization: Bearer " + token
		+ "\r\nMcp-Session-Id: " + session + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nContent-Length: "
		+ std::to_string(body.size()) + "\r\n\r\n" + body;
	background_post post(port, request, limit);

	// The call reaches the main thread and starts its job; the job is queued and never runs, so the call stays held.
	const auto deadline = std::chrono::steady_clock::now() + limit;
	while (last_job == 0 && std::chrono::steady_clock::now() < deadline) {
		tasks.drain_main(std::chrono::hours(1));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	BOOST_REQUIRE(last_job != 0);
	const edt::job_service::job_id job = last_job;
	BOOST_REQUIRE(jobs.is_running(job));
	// Held: the job runs and nothing has answered the POST.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	tasks.drain_main(std::chrono::hours(1));
	BOOST_TEST(!post.finished());
	BOOST_TEST(!jobs.info(job)->cancel_requested);

	server->stop_http();
	BOOST_TEST(!server->running());
	while (!post.finished() && std::chrono::steady_clock::now() < deadline + limit) {
		tasks.drain_main(std::chrono::hours(1));
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	tasks.drain_main(std::chrono::hours(1));

	// Cancelled: asked for while it waits in the queue, or already retired as cancelled.
	const std::optional<edt::job_service::job_info> stopped = jobs.info(job);
	BOOST_REQUIRE(stopped.has_value());
	BOOST_TEST((stopped->cancel_requested || stopped->status == edt::job_status::cancelled));
	// The POST was let go, not answered with the job's result.
	BOOST_TEST(post.finished());
	BOOST_TEST(!post.timed_out());
	const std::string received = post.received();
	BOOST_TEST(received.find("\"result\"") == std::string::npos);
	BOOST_TEST((received.empty() || received.starts_with("HTTP/1.1 202")));
	BOOST_TEST(closed == std::vector<std::string>{ session });

	// Nothing is held any more: tick() does nothing, and the job ends cancelled when the lanes run.
	server->tick();
	test_util::pump(tasks);
	BOOST_REQUIRE(jobs.info(job).has_value());
	BOOST_TEST((jobs.info(job)->status == edt::job_status::cancelled));
	server->tick();
	BOOST_TEST(closed.size() == 1u);

	server.reset();
	tasks.drain_main(std::chrono::hours(1));
	BOOST_TEST(closed.size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
