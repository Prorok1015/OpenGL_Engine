#include <boost/test/unit_test.hpp>
#include <mcp/mcp_server.h>
#include <boost/asio/buffer.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// mcp::server on a real loopback socket: 127.0.0.1, port 0 (the system picks a free one), blocking clients, no sleeps.
// The protocol itself is covered without a socket in mcp_endpoint_tests.cpp; this file is about the transport: the
// listener, the token and Host checks on the wire, the connection file, server-sent events and shutdown.

namespace {

namespace json = boost::json;
namespace net = boost::asio;
using tcp = net::ip::tcp;

json::value json_of(std::string_view text)
{
	return json::parse(text);
}

class echo_host final : public mcp::host
{
public:
	std::vector<mcp::tool_info> list_tools() override
	{
		mcp::tool_info tool;
		tool.name = "echo";
		tool.description = "Gives back its arguments";
		tool.read_only = true;
		return { tool };
	}

	void call_tool(std::string, json::value args, mcp::progress_sink progress, std::function<void(mcp::call_result)> done) override
	{
		progress(1.0, 2.0, "halfway");
		mcp::call_result result;
		result.structured = json::object{ { "echo", std::move(args) } };
		done(std::move(result));
	}

	std::vector<mcp::resource_info> list_resources() override { return {}; }
	void read_resource(std::string, std::function<void(mcp::resource_result)> done) override { done({}); }

	void log(mcp::level, std::string_view message) override
	{
		std::lock_guard lock(m_mutex);
		m_logged.emplace_back(message);
	}

	std::vector<std::string> logged() const
	{
		std::lock_guard lock(m_mutex);
		return m_logged;
	}

private:
	mutable std::mutex m_mutex;
	std::vector<std::string> m_logged;
};

// Closes the socket if a test hangs (a broken server would otherwise block it forever). A passing test never waits for it.
class watchdog
{
public:
	explicit watchdog(tcp::socket& socket, std::chrono::seconds limit = std::chrono::seconds(60))
		: m_thread([this, &socket, limit] {
			std::unique_lock lock(m_mutex);
			if (!m_cv.wait_for(lock, limit, [this] { return m_finished; })) {
				boost::system::error_code ignored;
				socket.close(ignored);
			}
		})
	{
	}

	~watchdog()
	{
		{
			std::lock_guard lock(m_mutex);
			m_finished = true;
		}
		m_cv.notify_all();
		m_thread.join();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_finished = false;
	std::thread m_thread;
};

struct reply
{
	int status = 0;
	std::map<std::string, std::string> headers;
	std::string body;
};

reply parse_reply(const std::string& raw)
{
	reply out;
	const std::size_t head_end = raw.find("\r\n\r\n");
	BOOST_REQUIRE_MESSAGE(head_end != std::string::npos, "no complete response head in: " + raw);
	const std::string head = raw.substr(0, head_end);
	out.body = raw.substr(head_end + 4);
	out.status = std::stoi(head.substr(head.find(' ') + 1, 3));
	std::size_t line_start = head.find("\r\n");
	while (line_start != std::string::npos) {
		const std::size_t next = head.find("\r\n", line_start + 2);
		const std::string line = head.substr(line_start + 2, next == std::string::npos ? std::string::npos : next - line_start - 2);
		const std::size_t colon = line.find(':');
		if (colon != std::string::npos) {
			std::size_t value = colon + 1;
			while (value < line.size() && line[value] == ' ') ++value;
			out.headers[line.substr(0, colon)] = line.substr(value);
		}
		line_start = next;
	}
	return out;
}

struct client
{
	explicit client(std::uint16_t port)
		: socket(io)
	{
		socket.connect(tcp::endpoint(net::ip::address_v4::loopback(), port));
	}

	void send(const std::string& text)
	{
		net::write(socket, net::buffer(text));
	}

	// Reads until the server closes the connection.
	std::string read_to_end()
	{
		std::string out;
		char chunk[4096];
		boost::system::error_code ec;
		while (!ec) {
			const std::size_t got = socket.read_some(net::buffer(chunk), ec);
			out.append(chunk, got);
		}
		return out;
	}

	// Reads until `marker` is in what came so far (or the connection ends).
	std::string read_until(const std::string& marker)
	{
		std::string out;
		char chunk[4096];
		boost::system::error_code ec;
		while (out.find(marker) == std::string::npos && !ec) {
			const std::size_t got = socket.read_some(net::buffer(chunk), ec);
			out.append(chunk, got);
		}
		return out;
	}

	net::io_context io;
	tcp::socket socket;
};

struct request_options
{
	std::string method = "POST";
	std::string host;       // empty: 127.0.0.1:<port>
	std::string token;      // empty: no Authorization header
	std::string session;    // empty: no Mcp-Session-Id header
	std::string accept = "application/json, text/event-stream";
	std::string body;
	// Content-Length to declare; negative: the size of the body.
	long long declared_length = -1;
};

std::string http_request(std::uint16_t port, const request_options& options)
{
	std::string text = options.method + " /mcp HTTP/1.1\r\n";
	text += "Host: " + (options.host.empty() ? "127.0.0.1:" + std::to_string(port) : options.host) + "\r\n";
	text += "Accept: " + options.accept + "\r\n";
	if (options.method == "POST") text += "Content-Type: application/json\r\n";
	if (!options.token.empty()) text += "Authorization: Bearer " + options.token + "\r\n";
	if (!options.session.empty()) text += "Mcp-Session-Id: " + options.session + "\r\n";
	if (options.method == "POST") {
		const long long length = options.declared_length >= 0 ? options.declared_length : static_cast<long long>(options.body.size());
		text += "Content-Length: " + std::to_string(length) + "\r\n";
	}
	text += "Connection: close\r\n\r\n";
	text += options.body;
	return text;
}

reply round_trip(std::uint16_t port, const request_options& options)
{
	client c(port);
	watchdog guard(c.socket);
	c.send(http_request(port, options));
	return parse_reply(c.read_to_end());
}

std::string rpc(std::string_view method, std::string_view params = {}, int id = 1)
{
	std::string out = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + std::string(method) + "\"";
	if (!params.empty()) out += R"(,"params":)" + std::string(params);
	return out + "}";
}

struct temp_dir
{
	temp_dir()
		: path(std::filesystem::temp_directory_path() / ("snk_mcp_test_" + std::to_string(counter()++)))
	{
		std::filesystem::remove_all(path);
		std::filesystem::create_directories(path);
	}

	~temp_dir()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}

	static int& counter()
	{
		static int value = 0;
		return value;
	}

	std::filesystem::path path;
};

struct server_fixture
{
	server_fixture()
		: server(host, make_config(dir.path / ".snk" / "mcp.json"))
	{
		BOOST_REQUIRE_MESSAGE(server.start(), server.error());
	}

	static mcp::server_config make_config(std::filesystem::path file)
	{
		mcp::server_config cfg;
		cfg.connection_file = std::move(file);
		cfg.max_message_bytes = 4096;
		cfg.api_version = "3";
		return cfg;
	}

	request_options with_token() const
	{
		request_options options;
		options.token = server.token();
		return options;
	}

	std::string initialize()
	{
		request_options options = with_token();
		options.body = rpc("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{}})");
		const reply answer = round_trip(server.port(), options);
		BOOST_REQUIRE_EQUAL(answer.status, 200);
		const auto session = answer.headers.find("Mcp-Session-Id");
		BOOST_REQUIRE(session != answer.headers.end());
		return session->second;
	}

	temp_dir dir;
	echo_host host;
	mcp::server server;
};

}

BOOST_AUTO_TEST_SUITE(McpServerSocketTests)

BOOST_FIXTURE_TEST_CASE(Start_BindsLoopbackAndWritesTheConnectionFile, server_fixture)
{
	BOOST_CHECK(server.running());
	BOOST_CHECK_NE(server.port(), 0);
	BOOST_CHECK_EQUAL(server.token().size(), 64u);
	BOOST_CHECK_EQUAL(server.url(), "http://127.0.0.1:" + std::to_string(server.port()) + "/mcp");

	const std::filesystem::path file = dir.path / ".snk" / "mcp.json";
	BOOST_REQUIRE(std::filesystem::exists(file));
	std::ifstream in(file, std::ios::binary);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const json::object info = json_of(text).as_object();
	BOOST_CHECK_EQUAL(std::string(info.at("url").as_string()), server.url());
	BOOST_CHECK_EQUAL(info.at("port").as_int64(), server.port());
	BOOST_CHECK_EQUAL(std::string(info.at("token").as_string()), server.token());

#ifndef _WIN32
	// Only the user may read the token.
	const auto permissions = std::filesystem::status(file).permissions();
	namespace fs = std::filesystem;
	BOOST_CHECK((permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
}

BOOST_FIXTURE_TEST_CASE(Start_ReplacesAnOldFileWithWiderRights, server_fixture)
{
	// A stale file from an earlier run keeps its mode through O_CREAT: start() must tighten it.
	server.stop();
	const std::filesystem::path file = dir.path / ".snk" / "mcp.json";
	{
		std::ofstream stale(file);
		stale << "old";
	}
#ifndef _WIN32
	std::filesystem::permissions(file, std::filesystem::perms::owner_all | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
#endif
	BOOST_REQUIRE(server.start());
	std::ifstream in(file, std::ios::binary);
	const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	BOOST_CHECK_EQUAL(std::string(json_of(text).as_object().at("token").as_string()), server.token());
#ifndef _WIN32
	namespace fs = std::filesystem;
	BOOST_CHECK((fs::status(file).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
}

BOOST_FIXTURE_TEST_CASE(Stop_RemovesTheFileAndIsIdempotent, server_fixture)
{
	const std::filesystem::path file = dir.path / ".snk" / "mcp.json";
	server.stop();
	BOOST_CHECK(!server.running());
	BOOST_CHECK(!std::filesystem::exists(file));
	server.stop();
	BOOST_CHECK(!server.running());
	// It can run again, on another port and with another token.
	BOOST_REQUIRE(server.start());
	BOOST_CHECK(server.running());
	BOOST_CHECK(std::filesystem::exists(file));
}

BOOST_AUTO_TEST_CASE(Start_TwoServersGetDifferentPortsAndTokens)
{
	echo_host host;
	mcp::server first(host, mcp::server_config{});
	mcp::server second(host, mcp::server_config{});
	BOOST_REQUIRE(first.start());
	BOOST_REQUIRE(second.start());
	BOOST_CHECK_NE(first.port(), second.port());
	BOOST_CHECK_NE(first.token(), second.token());
}

BOOST_AUTO_TEST_CASE(Start_ATakenPortFailsWithAMessage)
{
	echo_host host;
	mcp::server first(host, mcp::server_config{});
	BOOST_REQUIRE(first.start());

	mcp::server_config taken;
	taken.port = first.port();
	mcp::server second(host, taken);
	BOOST_CHECK(!second.start());
	BOOST_CHECK(!second.running());
	BOOST_CHECK(!second.error().empty());
	BOOST_CHECK(first.running());
}

BOOST_AUTO_TEST_CASE(Start_AFileThatCannotBeWrittenFailsAndLeavesNothingRunning)
{
	echo_host host;
	temp_dir dir;
	// The directory of the file is a regular file.
	{
		std::ofstream blocker(dir.path / "blocker");
		blocker << "x";
	}
	mcp::server_config cfg;
	cfg.connection_file = dir.path / "blocker" / "mcp.json";
	mcp::server server(host, cfg);
	BOOST_CHECK(!server.start());
	BOOST_CHECK(!server.running());
	BOOST_CHECK(!server.error().empty());
}

BOOST_FIXTURE_TEST_CASE(NoToken_GetsA401AndTheConnectionIsClosed, server_fixture)
{
	request_options options;
	options.body = rpc("ping");
	const reply answer = round_trip(server.port(), options);
	BOOST_CHECK_EQUAL(answer.status, 401);
	BOOST_CHECK_EQUAL(answer.headers.at("WWW-Authenticate"), "Bearer realm=\"snk-mcp\"");
	BOOST_CHECK_EQUAL(answer.headers.at("Connection"), "close");
}

BOOST_FIXTURE_TEST_CASE(WrongToken_GetsA401, server_fixture)
{
	request_options options;
	options.token = server.token() + "0";
	options.body = rpc("ping");
	BOOST_CHECK_EQUAL(round_trip(server.port(), options).status, 401);
}

BOOST_FIXTURE_TEST_CASE(ARefusedBodyIsNeverRead, server_fixture)
{
	// The head promises more than the limit and no body follows: the answer comes from the head alone.
	request_options options = with_token();
	options.declared_length = 100000;
	const reply answer = round_trip(server.port(), options);
	BOOST_CHECK_EQUAL(answer.status, 413);

	// The same without a token: the token is checked first, and the body (never sent) is not waited for.
	request_options anonymous;
	anonymous.declared_length = 100000;
	BOOST_CHECK_EQUAL(round_trip(server.port(), anonymous).status, 401);
}

BOOST_FIXTURE_TEST_CASE(RebindingHost_IsRefusedOnTheWire, server_fixture)
{
	request_options options = with_token();
	options.host = "evil.example:" + std::to_string(server.port());
	options.body = rpc("ping");
	BOOST_CHECK_EQUAL(round_trip(server.port(), options).status, 403);
}

BOOST_FIXTURE_TEST_CASE(FullSession_InitializeListCallPingDelete, server_fixture)
{
	const std::string session = initialize();

	request_options list = with_token();
	list.session = session;
	list.body = rpc("tools/list", {}, 2);
	const json::object listed = json_of(round_trip(server.port(), list).body).as_object();
	BOOST_REQUIRE_EQUAL(listed.at("result").as_object().at("tools").as_array().size(), 1u);
	BOOST_CHECK_EQUAL(std::string(listed.at("result").as_object().at("tools").as_array()[0].as_object().at("name").as_string()), "echo");

	request_options call = with_token();
	call.session = session;
	call.body = rpc("tools/call", R"({"name":"echo","arguments":{"n":5}})", 3);
	const reply called = round_trip(server.port(), call);
	BOOST_CHECK_EQUAL(called.status, 200);
	BOOST_CHECK_EQUAL(called.headers.at("Content-Type"), "application/json");
	const json::object outcome = json_of(called.body).as_object().at("result").as_object();
	BOOST_CHECK_EQUAL(outcome.at("structuredContent").as_object().at("echo").as_object().at("n").as_int64(), 5);

	request_options remove = with_token();
	remove.method = "DELETE";
	remove.session = session;
	BOOST_CHECK_EQUAL(round_trip(server.port(), remove).status, 200);
	BOOST_CHECK_EQUAL(round_trip(server.port(), list).status, 404);
}

BOOST_FIXTURE_TEST_CASE(ProgressToken_AnswersWithAnEventStreamThatEnds, server_fixture)
{
	const std::string session = initialize();
	request_options call = with_token();
	call.session = session;
	call.body = rpc("tools/call", R"({"name":"echo","arguments":{},"_meta":{"progressToken":"p1"}})", 4);
	const reply streamed = round_trip(server.port(), call);
	BOOST_CHECK_EQUAL(streamed.status, 200);
	BOOST_CHECK_EQUAL(streamed.headers.at("Content-Type"), "text/event-stream");
	const std::size_t progress = streamed.body.find("notifications/progress");
	const std::size_t response = streamed.body.find(R"("id":4)");
	BOOST_REQUIRE(progress != std::string::npos);
	BOOST_REQUIRE(response != std::string::npos);
	BOOST_CHECK_LT(progress, response);
	BOOST_CHECK_NE(streamed.body.find("event: message\ndata: "), std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(EventStream_DeliversNotificationsAndEndsWithTheSession, server_fixture)
{
	const std::string session = initialize();

	client stream(server.port());
	watchdog guard(stream.socket);
	request_options open = with_token();
	open.method = "GET";
	open.session = session;
	open.accept = "text/event-stream";
	stream.send(http_request(server.port(), open));
	// The comment line is written once the stream is registered: from here on notifications reach it.
	const std::string opened = stream.read_until(": stream open\n\n");
	BOOST_REQUIRE_NE(opened.find(": stream open"), std::string::npos);
	BOOST_CHECK_NE(opened.find("Content-Type: text/event-stream"), std::string::npos);

	server.notify_tools_changed();
	const std::string notified = stream.read_until("list_changed");
	BOOST_CHECK_NE(notified.find("notifications/tools/list_changed"), std::string::npos);

	request_options remove = with_token();
	remove.method = "DELETE";
	remove.session = session;
	BOOST_CHECK_EQUAL(round_trip(server.port(), remove).status, 200);
	// The server closes the stream: reading ends instead of waiting for more.
	stream.read_to_end();
}

BOOST_FIXTURE_TEST_CASE(Stop_EndsOpenStreamsAndRefusesNewConnections, server_fixture)
{
	const std::string session = initialize();
	client stream(server.port());
	watchdog guard(stream.socket);
	request_options open = with_token();
	open.method = "GET";
	open.session = session;
	open.accept = "text/event-stream";
	stream.send(http_request(server.port(), open));
	stream.read_until(": stream open\n\n");

	const std::uint16_t port = server.port();
	server.stop();
	stream.read_to_end(); // returns because stop() closed the connection
	BOOST_CHECK(!server.running());

	bool refused = false;
	try {
		client late(port);
	} catch (const boost::system::system_error&) {
		refused = true;
	}
	BOOST_CHECK(refused);
}

BOOST_FIXTURE_TEST_CASE(ManyClientsAtOnce_AreAllAnswered, server_fixture)
{
	const std::string session = initialize();
	std::vector<int> statuses(8, 0);
	std::vector<std::thread> clients;
	for (std::size_t i = 0; i < statuses.size(); ++i) {
		clients.emplace_back([&, i] {
			// No Boost.Test macros off the test thread.
			request_options ping = with_token();
			ping.session = session;
			ping.body = rpc("ping", {}, static_cast<int>(i) + 10);
			client c(server.port());
			c.send(http_request(server.port(), ping));
			const std::string raw = c.read_to_end();
			statuses[i] = raw.size() > 12 ? std::stoi(raw.substr(9, 3)) : 0;
		});
	}
	for (auto& worker : clients) worker.join();
	for (const int status : statuses) BOOST_CHECK_EQUAL(status, 200);
}

BOOST_AUTO_TEST_SUITE_END()
