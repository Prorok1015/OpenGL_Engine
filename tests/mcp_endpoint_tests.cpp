#include <boost/test/unit_test.hpp>
#include <mcp/mcp_endpoint.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// mcp::endpoint, the protocol layer of the editor's MCP server, against a fake host: no engine, no socket.
// The TCP transport has its own file (mcp_server_socket_tests.cpp).

namespace {

namespace json = boost::json;

constexpr const char* token = "secret-token-0123456789";
constexpr std::uint16_t bound_port = 49321;

json::value json_of(std::string_view text)
{
	return json::parse(text);
}

// Stands in for the editor: a fixed list of tools and resources, calls answered at once or held for the test.
class fake_host final : public mcp::host
{
public:
	struct held_call
	{
		std::string name;
		json::value args;
		mcp::progress_sink progress;
		std::function<void(mcp::call_result)> done;
	};

	std::vector<mcp::tool_info> list_tools() override { return tools; }

	void call_tool(std::string name, json::value args, mcp::progress_sink progress, std::function<void(mcp::call_result)> done) override
	{
		if (throw_on_call) throw std::runtime_error("boom");
		if (hold_calls) {
			held.push_back({ std::move(name), std::move(args), std::move(progress), std::move(done) });
			return;
		}
		last_name = name;
		last_args = args;
		done(next_result);
		if (answer_twice) done(next_result);
	}

	std::vector<mcp::resource_info> list_resources() override { return resources; }

	void read_resource(std::string uri, std::function<void(mcp::resource_result)> done) override
	{
		const auto found = contents.find(uri);
		if (found == contents.end()) {
			mcp::resource_result missing;
			done(std::move(missing));
			return;
		}
		done(found->second);
	}

	void log(mcp::level, std::string_view message) override { logged.emplace_back(message); }

	std::vector<mcp::tool_info> tools;
	std::vector<mcp::resource_info> resources;
	std::map<std::string, mcp::resource_result> contents;
	mcp::call_result next_result;
	bool hold_calls = false;
	bool answer_twice = false;
	bool throw_on_call = false;
	std::vector<held_call> held;
	std::string last_name;
	json::value last_args;
	std::vector<std::string> logged;
};

// Records what the endpoint writes to an event stream.
class fake_sink final : public mcp::stream_sink
{
public:
	void write(std::string_view chunk) override
	{
		std::lock_guard lock(m_mutex);
		m_text.append(chunk);
	}

	void close() override
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
	}

	std::string text() const
	{
		std::lock_guard lock(m_mutex);
		return m_text;
	}

	bool closed() const
	{
		std::lock_guard lock(m_mutex);
		return m_closed;
	}

	// The JSON messages of the stream, in order.
	std::vector<json::value> messages() const
	{
		std::vector<json::value> out;
		const std::string all = text();
		std::size_t pos = 0;
		while ((pos = all.find("data: ", pos)) != std::string::npos) {
			const std::size_t end = all.find('\n', pos);
			out.push_back(json::parse(all.substr(pos + 6, end - pos - 6)));
			pos = end;
		}
		return out;
	}

private:
	mutable std::mutex m_mutex;
	std::string m_text;
	bool m_closed = false;
};

using header_list = std::vector<std::pair<std::string, std::string>>;

struct response_box
{
	std::mutex mutex;
	std::vector<mcp::http_response> responses;

	mcp::reply_fn reply()
	{
		return [this](mcp::http_response response) {
			std::lock_guard lock(mutex);
			responses.push_back(std::move(response));
		};
	}

	std::size_t count()
	{
		std::lock_guard lock(mutex);
		return responses.size();
	}
};

std::string header_of(const mcp::http_response& response, std::string_view name)
{
	for (const auto& [key, value] : response.headers) {
		if (key == name) return value;
	}
	return {};
}

struct endpoint_fixture
{
	endpoint_fixture() : endpoint_fixture(default_config()) {}

	explicit endpoint_fixture(mcp::server_config config)
		: cfg(std::move(config))
		, ep(host, cfg)
	{
		ep.set_port(bound_port);
		mcp::tool_info echo;
		echo.name = "log_read";
		echo.description = "Read the log";
		echo.input_schema = json_of(R"({"type":"object","properties":{"limit":{"type":"integer"}}})");
		echo.output_schema = json_of(R"({"type":"object"})");
		echo.read_only = true;
		host.tools.push_back(echo);
		host.next_result.structured = json_of(R"({"entries":[],"next_seq":1})");

		mcp::resource_info log;
		log.uri = "snk://log";
		log.name = "Engine log";
		log.mime_type = "application/json";
		host.resources.push_back(log);
		mcp::resource_result log_contents;
		log_contents.found = true;
		log_contents.contents.push_back({ "snk://log", "application/json", R"({"entries":[]})", {}, false });
		host.contents["snk://log"] = log_contents;
	}

	static mcp::server_config default_config()
	{
		mcp::server_config cfg;
		cfg.token = token;
		cfg.api_version = "7";
		cfg.server_version = "1.2";
		return cfg;
	}

	mcp::http_request make(std::string method, std::string body = {}, header_list extra = {}) const
	{
		mcp::http_request request;
		request.method = std::move(method);
		request.target = "/mcp";
		request.body = std::move(body);
		request.headers = {
			{ "Host", "127.0.0.1:" + std::to_string(bound_port) },
			{ "Authorization", std::string("Bearer ") + token },
			{ "Content-Type", "application/json" },
			{ "Accept", "application/json, text/event-stream" } };
		for (auto& header : extra) request.headers.push_back(std::move(header));
		return request;
	}

	static std::string rpc(std::string_view method, std::string_view params, int id = 1)
	{
		std::string out = R"({"jsonrpc":"2.0","id":)" + std::to_string(id) + R"(,"method":")" + std::string(method) + "\"";
		if (!params.empty()) out += R"(,"params":)" + std::string(params);
		return out + "}";
	}

	// One request answered at once.
	mcp::http_response send(mcp::http_request request, std::shared_ptr<mcp::stream_sink> sink = nullptr)
	{
		response_box box;
		ep.handle(std::move(request), box.reply(), std::move(sink));
		BOOST_REQUIRE_EQUAL(box.responses.size(), 1u);
		return box.responses.front();
	}

	std::string initialize(std::string_view version = "2025-06-18")
	{
		const auto response = send(make("POST", rpc("initialize", R"({"protocolVersion":")" + std::string(version) + R"(","capabilities":{},"clientInfo":{"name":"t","version":"1"}})")));
		BOOST_REQUIRE_EQUAL(response.status, 200);
		const std::string session = header_of(response, "Mcp-Session-Id");
		BOOST_REQUIRE(!session.empty());
		return session;
	}

	mcp::http_response call(const std::string& session, std::string_view method, std::string_view params = {}, int id = 1,
		std::shared_ptr<mcp::stream_sink> sink = nullptr)
	{
		return send(make("POST", rpc(method, params, id), { { "Mcp-Session-Id", session } }), std::move(sink));
	}

	json::value result_of(const mcp::http_response& response)
	{
		BOOST_REQUIRE_EQUAL(response.status, 200);
		const json::value body = json::parse(response.body);
		BOOST_REQUIRE(body.as_object().contains("result"));
		return body.as_object().at("result");
	}

	std::int64_t error_code_of(const mcp::http_response& response)
	{
		const json::value body = json::parse(response.body);
		BOOST_REQUIRE(body.as_object().contains("error"));
		return body.as_object().at("error").as_object().at("code").as_int64();
	}

	fake_host host;
	mcp::server_config cfg;
	mcp::endpoint ep;
};

}

BOOST_AUTO_TEST_SUITE(McpEndpointSecurityTests)

BOOST_FIXTURE_TEST_CASE(NoToken_IsRefusedAndTheConnectionClosed, endpoint_fixture)
{
	mcp::http_request request = make("POST", rpc("ping", {}));
	std::erase_if(request.headers, [](const auto& header) { return header.first == "Authorization"; });
	const auto response = send(std::move(request));
	BOOST_CHECK_EQUAL(response.status, 401);
	BOOST_CHECK(response.close);
	BOOST_CHECK_EQUAL(header_of(response, "WWW-Authenticate"), "Bearer realm=\"snk-mcp\"");
	BOOST_CHECK_EQUAL(ep.session_count(), 0u);
}

BOOST_FIXTURE_TEST_CASE(WrongToken_IsRefused, endpoint_fixture)
{
	for (const std::string guess : { std::string("nope"), std::string(token) + "x", std::string(token).substr(1), std::string(std::string_view(token).size(), 'a'), std::string() }) {
		mcp::http_request request = make("POST", rpc("ping", {}));
		for (auto& header : request.headers) {
			if (header.first == "Authorization") header.second = "Bearer " + guess;
		}
		BOOST_CHECK_EQUAL(send(std::move(request)).status, 401);
	}
	// Another scheme with the right secret is not a bearer token.
	mcp::http_request basic = make("POST", rpc("ping", {}));
	for (auto& header : basic.headers) {
		if (header.first == "Authorization") header.second = std::string("Basic ") + token;
	}
	BOOST_CHECK_EQUAL(send(std::move(basic)).status, 401);
}

BOOST_FIXTURE_TEST_CASE(BearerSchemeIsCaseInsensitive, endpoint_fixture)
{
	mcp::http_request request = make("POST", rpc("ping", {}));
	for (auto& header : request.headers) {
		if (header.first == "Authorization") header.second = std::string("bearer ") + token;
	}
	BOOST_CHECK_EQUAL(send(std::move(request)).status, 400); // past the token check: no session yet
}

BOOST_AUTO_TEST_CASE(EmptyConfiguredToken_RefusesEverything)
{
	mcp::server_config open;
	open.token.clear();
	endpoint_fixture f(open);
	mcp::http_request request = f.make("POST", endpoint_fixture::rpc("ping", {}));
	for (auto& header : request.headers) {
		if (header.first == "Authorization") header.second = "Bearer ";
	}
	BOOST_CHECK_EQUAL(f.send(std::move(request)).status, 401);
}

BOOST_FIXTURE_TEST_CASE(HostHeader_MustNameThisServer, endpoint_fixture)
{
	const auto status_with_host = [&](std::string host_value) {
		mcp::http_request request = make("POST", rpc("ping", {}));
		for (auto& header : request.headers) {
			if (header.first == "Host") header.second = std::move(host_value);
		}
		return send(std::move(request)).status;
	};
	// DNS rebinding: the browser sends the attacker's name in Host.
	BOOST_CHECK_EQUAL(status_with_host("evil.example:" + std::to_string(bound_port)), 403);
	BOOST_CHECK_EQUAL(status_with_host("evil.example"), 403);
	BOOST_CHECK_EQUAL(status_with_host("127.0.0.1:1"), 403);
	BOOST_CHECK_EQUAL(status_with_host("127.0.0.1"), 403);
	BOOST_CHECK_EQUAL(status_with_host("127.0.0.1.evil.example:" + std::to_string(bound_port)), 403);
	BOOST_CHECK_EQUAL(status_with_host("[::1]:abc"), 403);
	BOOST_CHECK_EQUAL(status_with_host(""), 403);
	// The loopback names are fine and get past to the session check.
	BOOST_CHECK_EQUAL(status_with_host("localhost:" + std::to_string(bound_port)), 400);
	BOOST_CHECK_EQUAL(status_with_host("LOCALHOST:" + std::to_string(bound_port)), 400);
	BOOST_CHECK_EQUAL(status_with_host("[::1]:" + std::to_string(bound_port)), 400);
	BOOST_CHECK_EQUAL(status_with_host("127.0.0.1:" + std::to_string(bound_port)), 400);
}

BOOST_FIXTURE_TEST_CASE(MissingHostHeader_IsRefused, endpoint_fixture)
{
	mcp::http_request request = make("POST", rpc("ping", {}));
	std::erase_if(request.headers, [](const auto& header) { return header.first == "Host"; });
	BOOST_CHECK_EQUAL(send(std::move(request)).status, 403);
}

BOOST_FIXTURE_TEST_CASE(Origin_OnlyLoopbackPagesAndConfiguredOnes, endpoint_fixture)
{
	const auto status_with_origin = [&](std::string origin) {
		return send(make("POST", rpc("ping", {}), { { "Origin", std::move(origin) } })).status;
	};
	BOOST_CHECK_EQUAL(status_with_origin("https://evil.example"), 403);
	BOOST_CHECK_EQUAL(status_with_origin("http://evil.example:80"), 403);
	BOOST_CHECK_EQUAL(status_with_origin("http://localhost.evil.example"), 403);
	BOOST_CHECK_EQUAL(status_with_origin("null"), 403);
	BOOST_CHECK_EQUAL(status_with_origin("file://"), 403);
	BOOST_CHECK_EQUAL(status_with_origin("http://localhost:6274/path"), 403);
	// A page served from the machine itself (the MCP Inspector) is fine, on any port.
	BOOST_CHECK_EQUAL(status_with_origin("http://localhost:6274"), 400);
	BOOST_CHECK_EQUAL(status_with_origin("http://127.0.0.1:3000"), 400);
	BOOST_CHECK_EQUAL(status_with_origin("https://[::1]"), 400);
}

BOOST_AUTO_TEST_CASE(Origin_ConfiguredExtraOriginIsAllowed)
{
	mcp::server_config cfg = endpoint_fixture::default_config();
	cfg.allowed_origins = { "https://tools.example" };
	endpoint_fixture f(cfg);
	BOOST_CHECK_EQUAL(f.send(f.make("POST", endpoint_fixture::rpc("ping", {}), { { "Origin", "https://tools.example" } })).status, 400);
	BOOST_CHECK_EQUAL(f.send(f.make("POST", endpoint_fixture::rpc("ping", {}), { { "Origin", "https://other.example" } })).status, 403);
}

BOOST_FIXTURE_TEST_CASE(Screen_RefusesBeforeTheBodyIsRead, endpoint_fixture)
{
	mcp::http_request head = make("POST");
	BOOST_CHECK(!ep.screen(head, 10).has_value());

	std::erase_if(head.headers, [](const auto& header) { return header.first == "Authorization"; });
	const auto unauthorized = ep.screen(head, 10);
	BOOST_REQUIRE(unauthorized.has_value());
	BOOST_CHECK_EQUAL(unauthorized->status, 401);
	BOOST_CHECK(unauthorized->close);

	const auto too_big = ep.screen(make("POST"), cfg.max_message_bytes + 1);
	BOOST_REQUIRE(too_big.has_value());
	BOOST_CHECK_EQUAL(too_big->status, 413);
	BOOST_CHECK(!ep.screen(make("POST"), cfg.max_message_bytes).has_value());
}

BOOST_FIXTURE_TEST_CASE(PathMethodAndContentType_AreChecked, endpoint_fixture)
{
	mcp::http_request wrong_path = make("POST", rpc("ping", {}));
	wrong_path.target = "/other";
	BOOST_CHECK_EQUAL(send(std::move(wrong_path)).status, 404);

	// A query string does not change the path.
	mcp::http_request with_query = make("POST", rpc("ping", {}));
	with_query.target = "/mcp?x=1";
	BOOST_CHECK_EQUAL(send(std::move(with_query)).status, 400);

	const auto put = send(make("PUT"));
	BOOST_CHECK_EQUAL(put.status, 405);
	BOOST_CHECK_EQUAL(header_of(put, "Allow"), "GET, POST, DELETE");

	mcp::http_request plain = make("POST", rpc("ping", {}));
	for (auto& header : plain.headers) {
		if (header.first == "Content-Type") header.second = "text/plain";
	}
	BOOST_CHECK_EQUAL(send(std::move(plain)).status, 415);

	mcp::http_request charset = make("POST", rpc("ping", {}));
	for (auto& header : charset.headers) {
		if (header.first == "Content-Type") header.second = "application/json; charset=utf-8";
	}
	BOOST_CHECK_EQUAL(send(std::move(charset)).status, 400);
}

BOOST_FIXTURE_TEST_CASE(HandleScreensToo_ABodyOverTheLimitIsRefused, endpoint_fixture)
{
	const auto response = send(make("POST", std::string(cfg.max_message_bytes + 1, ' ')));
	BOOST_CHECK_EQUAL(response.status, 413);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(McpEndpointProtocolTests)

BOOST_FIXTURE_TEST_CASE(Initialize_AnswersWithCapabilitiesAndASession, endpoint_fixture)
{
	const auto response = send(make("POST", rpc("initialize", R"({"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}})")));
	BOOST_CHECK_EQUAL(header_of(response, "Content-Type"), "application/json");
	BOOST_CHECK_EQUAL(header_of(response, "Mcp-Session-Id").size(), 32u);
	const json::object result = result_of(response).as_object();
	BOOST_CHECK_EQUAL(std::string(result.at("protocolVersion").as_string()), "2025-06-18");
	BOOST_CHECK(result.at("capabilities").as_object().at("tools").as_object().at("listChanged").as_bool());
	BOOST_CHECK(result.at("capabilities").as_object().at("resources").as_object().at("subscribe").as_bool());
	BOOST_CHECK_EQUAL(std::string(result.at("serverInfo").as_object().at("name").as_string()), "snk-editor");
	BOOST_CHECK_EQUAL(std::string(result.at("serverInfo").as_object().at("version").as_string()), "1.2");
	BOOST_CHECK_EQUAL(std::string(result.at("serverInfo").as_object().at("snk_api").as_string()), "7");
	BOOST_CHECK(!result.contains("instructions"));
	BOOST_CHECK_EQUAL(json::parse(response.body).as_object().at("id").as_int64(), 1);
	BOOST_CHECK_EQUAL(ep.session_count(), 1u);
}

BOOST_FIXTURE_TEST_CASE(Initialize_SessionIdsDiffer, endpoint_fixture)
{
	BOOST_CHECK_NE(initialize(), initialize());
	BOOST_CHECK_EQUAL(ep.session_count(), 2u);
}

BOOST_FIXTURE_TEST_CASE(Initialize_NegotiatesTheProtocolVersion, endpoint_fixture)
{
	const auto version_for = [&](std::string_view asked) {
		const auto response = send(make("POST", rpc("initialize", R"({"protocolVersion":")" + std::string(asked) + R"(","capabilities":{}})")));
		return std::string(result_of(response).as_object().at("protocolVersion").as_string());
	};
	BOOST_CHECK_EQUAL(version_for("2025-03-26"), "2025-03-26");
	BOOST_CHECK_EQUAL(version_for("2025-06-18"), "2025-06-18");
	BOOST_CHECK_EQUAL(version_for("2099-01-01"), "2025-06-18"); // a newer one: the latest this server speaks
}

BOOST_AUTO_TEST_CASE(Initialize_ReportsInstructionsWhenConfigured)
{
	mcp::server_config cfg = endpoint_fixture::default_config();
	cfg.instructions = "Read only.";
	endpoint_fixture f(cfg);
	const auto response = f.send(f.make("POST", endpoint_fixture::rpc("initialize", R"({"protocolVersion":"2025-06-18"})")));
	BOOST_CHECK_EQUAL(std::string(f.result_of(response).as_object().at("instructions").as_string()), "Read only.");
}

BOOST_FIXTURE_TEST_CASE(Initialize_WithoutParamsIsInvalid, endpoint_fixture)
{
	const auto response = send(make("POST", rpc("initialize", {})));
	BOOST_CHECK_EQUAL(error_code_of(response), -32602);
	BOOST_CHECK_EQUAL(ep.session_count(), 0u);
}

BOOST_FIXTURE_TEST_CASE(Requests_NeedASession, endpoint_fixture)
{
	BOOST_CHECK_EQUAL(send(make("POST", rpc("tools/list", {}))).status, 400);
	const auto unknown = send(make("POST", rpc("tools/list", {}), { { "Mcp-Session-Id", "0123456789abcdef0123456789abcdef" } }));
	BOOST_CHECK_EQUAL(unknown.status, 404);
	BOOST_CHECK_EQUAL(error_code_of(unknown), -32600);
}

BOOST_FIXTURE_TEST_CASE(ProtocolVersionHeader_MustBeSupported, endpoint_fixture)
{
	const std::string session = initialize();
	const auto bad = send(make("POST", rpc("ping", {}), { { "Mcp-Session-Id", session }, { "MCP-Protocol-Version", "1999-01-01" } }));
	BOOST_CHECK_EQUAL(bad.status, 400);
	const auto good = send(make("POST", rpc("ping", {}), { { "Mcp-Session-Id", session }, { "MCP-Protocol-Version", "2025-03-26" } }));
	BOOST_CHECK_EQUAL(good.status, 200);
}

BOOST_FIXTURE_TEST_CASE(MalformedMessages_GetJsonRpcErrors, endpoint_fixture)
{
	const auto broken = send(make("POST", "{not json"));
	BOOST_CHECK_EQUAL(broken.status, 400);
	BOOST_CHECK_EQUAL(error_code_of(broken), -32700);
	BOOST_CHECK(json::parse(broken.body).as_object().at("id").is_null());

	const auto batch = send(make("POST", R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])"));
	BOOST_CHECK_EQUAL(batch.status, 400);
	BOOST_CHECK_EQUAL(error_code_of(batch), -32600);

	BOOST_CHECK_EQUAL(error_code_of(send(make("POST", R"({"jsonrpc":"2.0","id":1})"))), -32600);
	BOOST_CHECK_EQUAL(error_code_of(send(make("POST", R"({"jsonrpc":"2.0","id":1,"method":5})"))), -32600);
	BOOST_CHECK_EQUAL(error_code_of(send(make("POST", R"({"jsonrpc":"2.0","id":{"a":1},"method":"ping"})"))), -32600);

	// Deeper than the parser allows.
	std::string deep(200, '[');
	deep += std::string(200, ']');
	BOOST_CHECK_EQUAL(send(make("POST", deep)).status, 400);
}

BOOST_FIXTURE_TEST_CASE(Notifications_AndClientResponses_GetAccepted, endpoint_fixture)
{
	const std::string session = initialize();
	const auto initialized = send(make("POST", R"({"jsonrpc":"2.0","method":"notifications/initialized"})", { { "Mcp-Session-Id", session } }));
	BOOST_CHECK_EQUAL(initialized.status, 202);
	BOOST_CHECK(initialized.body.empty());

	const auto answer = send(make("POST", R"({"jsonrpc":"2.0","id":9,"result":{}})", { { "Mcp-Session-Id", session } }));
	BOOST_CHECK_EQUAL(answer.status, 202);
}

BOOST_FIXTURE_TEST_CASE(Ping_AnswersWithAnEmptyResult, endpoint_fixture)
{
	const std::string session = initialize();
	const auto response = call(session, "ping", {}, 41);
	BOOST_CHECK(result_of(response).as_object().empty());
	BOOST_CHECK_EQUAL(json::parse(response.body).as_object().at("id").as_int64(), 41);
}

BOOST_FIXTURE_TEST_CASE(StringIds_AreEchoed, endpoint_fixture)
{
	const std::string session = initialize();
	const auto response = send(make("POST", R"({"jsonrpc":"2.0","id":"abc","method":"ping"})", { { "Mcp-Session-Id", session } }));
	BOOST_CHECK_EQUAL(std::string(json::parse(response.body).as_object().at("id").as_string()), "abc");
}

BOOST_FIXTURE_TEST_CASE(UnknownMethod_IsMethodNotFound, endpoint_fixture)
{
	const std::string session = initialize();
	BOOST_CHECK_EQUAL(error_code_of(call(session, "prompts/list")), -32601);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "logging/setLevel", R"({"level":"debug"})")), -32601);
}

BOOST_FIXTURE_TEST_CASE(ToolsList_PublishesSchemasAndAnnotations, endpoint_fixture)
{
	const std::string session = initialize();
	const json::object result = result_of(call(session, "tools/list")).as_object();
	const json::array& tools = result.at("tools").as_array();
	BOOST_REQUIRE_EQUAL(tools.size(), 1u);
	const json::object& tool = tools[0].as_object();
	BOOST_CHECK_EQUAL(std::string(tool.at("name").as_string()), "log_read");
	BOOST_CHECK_EQUAL(std::string(tool.at("description").as_string()), "Read the log");
	BOOST_CHECK_EQUAL(std::string(tool.at("inputSchema").as_object().at("type").as_string()), "object");
	BOOST_CHECK(tool.contains("outputSchema"));
	BOOST_CHECK(tool.at("annotations").as_object().at("readOnlyHint").as_bool());
	BOOST_CHECK(!tool.at("annotations").as_object().at("destructiveHint").as_bool());
}

BOOST_FIXTURE_TEST_CASE(ToolsList_ASchemalessToolStillHasAnObjectInputSchema, endpoint_fixture)
{
	host.tools.clear();
	mcp::tool_info bare;
	bare.name = "bare";
	host.tools.push_back(bare);
	const std::string session = initialize();
	const json::object tool = result_of(call(session, "tools/list")).as_object().at("tools").as_array()[0].as_object();
	BOOST_CHECK_EQUAL(std::string(tool.at("inputSchema").as_object().at("type").as_string()), "object");
	BOOST_CHECK(!tool.contains("outputSchema"));
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_ReturnsStructuredContentAndATextCopy, endpoint_fixture)
{
	const std::string session = initialize();
	const auto response = call(session, "tools/call", R"({"name":"log_read","arguments":{"limit":3}})", 5);
	const json::object result = result_of(response).as_object();
	BOOST_CHECK(!result.at("isError").as_bool());
	BOOST_CHECK_EQUAL(result.at("structuredContent").as_object().at("next_seq").as_int64(), 1);
	const json::array& content = result.at("content").as_array();
	BOOST_REQUIRE_EQUAL(content.size(), 1u);
	BOOST_CHECK_EQUAL(std::string(content[0].as_object().at("type").as_string()), "text");
	BOOST_CHECK_EQUAL(json::parse(std::string(content[0].as_object().at("text").as_string())), host.next_result.structured);
	BOOST_CHECK_EQUAL(host.last_name, "log_read");
	BOOST_CHECK_EQUAL(host.last_args.as_object().at("limit").as_int64(), 3);
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_ArgumentsDefaultToAnEmptyObject, endpoint_fixture)
{
	const std::string session = initialize();
	call(session, "tools/call", R"({"name":"log_read"})");
	BOOST_CHECK(host.last_args.is_object());
	BOOST_CHECK(host.last_args.as_object().empty());
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_ImageContent, endpoint_fixture)
{
	host.next_result = {};
	host.next_result.items.push_back(mcp::content::make_image("QUJD", "image/png"));
	host.next_result.items.push_back(mcp::content::make_text("128x64"));
	const std::string session = initialize();
	const json::object result = result_of(call(session, "tools/call", R"({"name":"log_read"})")).as_object();
	const json::array& content = result.at("content").as_array();
	BOOST_REQUIRE_EQUAL(content.size(), 2u);
	BOOST_CHECK_EQUAL(std::string(content[0].as_object().at("type").as_string()), "image");
	BOOST_CHECK_EQUAL(std::string(content[0].as_object().at("data").as_string()), "QUJD");
	BOOST_CHECK_EQUAL(std::string(content[0].as_object().at("mimeType").as_string()), "image/png");
	BOOST_CHECK_EQUAL(std::string(content[1].as_object().at("text").as_string()), "128x64");
	BOOST_CHECK(!result.contains("structuredContent"));
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_AToolErrorIsAResultNotAProtocolError, endpoint_fixture)
{
	host.next_result = {};
	host.next_result.is_error = true;
	host.next_result.structured = json::object{ { "ignored", true } };
	host.next_result.items.push_back(mcp::content::make_text("no such viewport"));
	const std::string session = initialize();
	const auto response = call(session, "tools/call", R"({"name":"log_read"})");
	BOOST_CHECK_EQUAL(response.status, 200);
	const json::object result = result_of(response).as_object();
	BOOST_CHECK(result.at("isError").as_bool());
	BOOST_CHECK(!result.contains("structuredContent"));
	BOOST_CHECK_EQUAL(std::string(result.at("content").as_array()[0].as_object().at("text").as_string()), "no such viewport");
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_BadRequestsAreInvalidParams, endpoint_fixture)
{
	const std::string session = initialize();
	BOOST_CHECK_EQUAL(error_code_of(call(session, "tools/call", R"({"name":"nope"})")), -32602);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "tools/call", R"({})")), -32602);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "tools/call", R"({"name":5})")), -32602);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "tools/call", R"({"name":"log_read","arguments":[1]})")), -32602);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "tools/call")), -32602);
	BOOST_CHECK(host.last_name.empty()); // the host was never asked
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_AnAnswerThatComesLaterIsDelivered, endpoint_fixture)
{
	host.hold_calls = true;
	const std::string session = initialize();
	response_box box;
	ep.handle(make("POST", rpc("tools/call", R"({"name":"log_read"})", 8), { { "Mcp-Session-Id", session } }), box.reply(), nullptr);
	BOOST_CHECK_EQUAL(box.count(), 0u);
	BOOST_REQUIRE_EQUAL(host.held.size(), 1u);

	// Another thread answers, the way the editor's waiter thread does.
	std::thread worker([&] { host.held[0].done(host.next_result); });
	worker.join();
	BOOST_REQUIRE_EQUAL(box.count(), 1u);
	BOOST_CHECK_EQUAL(json::parse(box.responses[0].body).as_object().at("id").as_int64(), 8);
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_ASecondAnswerIsIgnored, endpoint_fixture)
{
	host.answer_twice = true;
	const std::string session = initialize();
	response_box box;
	ep.handle(make("POST", rpc("tools/call", R"({"name":"log_read"})"), { { "Mcp-Session-Id", session } }), box.reply(), nullptr);
	BOOST_CHECK_EQUAL(box.count(), 1u);
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_AThrowingHostBecomesAToolError, endpoint_fixture)
{
	host.throw_on_call = true;
	const std::string session = initialize();
	const json::object result = result_of(call(session, "tools/call", R"({"name":"log_read"})")).as_object();
	BOOST_CHECK(result.at("isError").as_bool());
}

BOOST_AUTO_TEST_CASE(ToolsCall_LimitsTheCallsInFlight)
{
	mcp::server_config cfg = endpoint_fixture::default_config();
	cfg.max_in_flight = 1;
	endpoint_fixture f(cfg);
	f.host.hold_calls = true;
	const std::string session = f.initialize();

	response_box first;
	f.ep.handle(f.make("POST", endpoint_fixture::rpc("tools/call", R"({"name":"log_read"})", 1), { { "Mcp-Session-Id", session } }), first.reply(), nullptr);
	BOOST_REQUIRE_EQUAL(f.host.held.size(), 1u);

	const auto busy = f.call(session, "tools/call", R"({"name":"log_read"})", 2);
	BOOST_CHECK_EQUAL(f.error_code_of(busy), -32000);
	BOOST_CHECK_EQUAL(f.host.held.size(), 1u);

	f.host.held[0].done(f.host.next_result);
	BOOST_CHECK_EQUAL(first.count(), 1u);
	// The slot is free again.
	response_box third;
	f.ep.handle(f.make("POST", endpoint_fixture::rpc("tools/call", R"({"name":"log_read"})", 3), { { "Mcp-Session-Id", session } }), third.reply(), nullptr);
	BOOST_CHECK_EQUAL(f.host.held.size(), 2u);
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_WithAProgressTokenStreamsProgressThenTheResponse, endpoint_fixture)
{
	host.hold_calls = true;
	const std::string session = initialize();
	const auto sink = std::make_shared<fake_sink>();
	response_box box;
	ep.handle(make("POST", rpc("tools/call", R"({"name":"log_read","_meta":{"progressToken":"tok-1"}})", 12), { { "Mcp-Session-Id", session } }), box.reply(), sink);

	// The stream starts at once; the answer comes later.
	BOOST_REQUIRE_EQUAL(box.count(), 1u);
	BOOST_CHECK(box.responses[0].stream);
	BOOST_CHECK_EQUAL(box.responses[0].status, 200);
	BOOST_CHECK_EQUAL(header_of(box.responses[0], "Content-Type"), "text/event-stream");
	BOOST_REQUIRE_EQUAL(host.held.size(), 1u);

	host.held[0].progress(1.0, 4.0, "step one");
	host.held[0].progress(2.0, std::nullopt, "");
	host.held[0].done(host.next_result);
	host.held[0].progress(3.0, 4.0, "too late");

	const auto messages = sink->messages();
	BOOST_REQUIRE_EQUAL(messages.size(), 3u);
	const json::object& first = messages[0].as_object();
	BOOST_CHECK_EQUAL(std::string(first.at("method").as_string()), "notifications/progress");
	BOOST_CHECK_EQUAL(std::string(first.at("params").as_object().at("progressToken").as_string()), "tok-1");
	BOOST_CHECK_EQUAL(first.at("params").as_object().at("progress").to_number<double>(), 1.0);
	BOOST_CHECK_EQUAL(first.at("params").as_object().at("total").to_number<double>(), 4.0);
	BOOST_CHECK_EQUAL(std::string(first.at("params").as_object().at("message").as_string()), "step one");
	BOOST_CHECK(!messages[1].as_object().at("params").as_object().contains("total"));
	BOOST_CHECK_EQUAL(messages[2].as_object().at("id").as_int64(), 12);
	BOOST_CHECK(messages[2].as_object().contains("result"));
	BOOST_CHECK(sink->closed());
	BOOST_CHECK_EQUAL(box.count(), 1u);
}

BOOST_FIXTURE_TEST_CASE(ToolsCall_ProgressTokenWithoutAStreamFallsBackToJson, endpoint_fixture)
{
	const std::string session = initialize();
	const auto response = call(session, "tools/call", R"({"name":"log_read","_meta":{"progressToken":7}})", 3, nullptr);
	BOOST_CHECK(!response.stream);
	BOOST_CHECK(result_of(response).as_object().contains("content"));
}

BOOST_FIXTURE_TEST_CASE(Resources_ListAndRead, endpoint_fixture)
{
	const std::string session = initialize();
	const json::array resources = result_of(call(session, "resources/list")).as_object().at("resources").as_array();
	BOOST_REQUIRE_EQUAL(resources.size(), 1u);
	BOOST_CHECK_EQUAL(std::string(resources[0].as_object().at("uri").as_string()), "snk://log");
	BOOST_CHECK_EQUAL(std::string(resources[0].as_object().at("mimeType").as_string()), "application/json");

	const json::array contents = result_of(call(session, "resources/read", R"({"uri":"snk://log"})")).as_object().at("contents").as_array();
	BOOST_REQUIRE_EQUAL(contents.size(), 1u);
	BOOST_CHECK_EQUAL(std::string(contents[0].as_object().at("uri").as_string()), "snk://log");
	BOOST_CHECK_EQUAL(std::string(contents[0].as_object().at("text").as_string()), R"({"entries":[]})");
	BOOST_CHECK(!contents[0].as_object().contains("blob"));
}

BOOST_FIXTURE_TEST_CASE(Resources_BlobsAreBase64, endpoint_fixture)
{
	mcp::resource_result picture;
	picture.found = true;
	picture.contents.push_back({ "snk://viewport/scene.png", "image/png", {}, "QUJD", true });
	host.contents["snk://viewport/scene.png"] = picture;
	const std::string session = initialize();
	const json::array contents = result_of(call(session, "resources/read", R"({"uri":"snk://viewport/scene.png"})")).as_object().at("contents").as_array();
	BOOST_CHECK_EQUAL(std::string(contents[0].as_object().at("blob").as_string()), "QUJD");
	BOOST_CHECK_EQUAL(std::string(contents[0].as_object().at("mimeType").as_string()), "image/png");
	BOOST_CHECK(!contents[0].as_object().contains("text"));
}

BOOST_FIXTURE_TEST_CASE(Resources_UnknownOrMalformedReads, endpoint_fixture)
{
	const std::string session = initialize();
	BOOST_CHECK_EQUAL(error_code_of(call(session, "resources/read", R"({"uri":"snk://nope"})")), -32002);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "resources/read", R"({})")), -32602);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "resources/read")), -32602);
	BOOST_CHECK(result_of(call(session, "resources/templates/list")).as_object().at("resourceTemplates").as_array().empty());
}

BOOST_FIXTURE_TEST_CASE(Subscribe_OnlyListedResources, endpoint_fixture)
{
	const std::string session = initialize();
	BOOST_CHECK(result_of(call(session, "resources/subscribe", R"({"uri":"snk://log"})")).as_object().empty());
	BOOST_CHECK_EQUAL(error_code_of(call(session, "resources/subscribe", R"({"uri":"snk://nope"})")), -32002);
	BOOST_CHECK_EQUAL(error_code_of(call(session, "resources/subscribe", R"({})")), -32602);
	BOOST_CHECK(result_of(call(session, "resources/unsubscribe", R"({"uri":"snk://log"})")).as_object().empty());
}

BOOST_FIXTURE_TEST_CASE(EventStream_NeedsAnAcceptHeaderAndASession, endpoint_fixture)
{
	const std::string session = initialize();
	const auto sink = std::make_shared<fake_sink>();

	mcp::http_request json_only = make("GET", {}, { { "Mcp-Session-Id", session } });
	for (auto& header : json_only.headers) {
		if (header.first == "Accept") header.second = "application/json";
	}
	BOOST_CHECK_EQUAL(send(std::move(json_only), sink).status, 406);
	BOOST_CHECK_EQUAL(send(make("GET"), sink).status, 400);
	BOOST_CHECK_EQUAL(send(make("GET", {}, { { "Mcp-Session-Id", "0123456789abcdef0123456789abcdef" } }), sink).status, 404);
	BOOST_CHECK_EQUAL(send(make("GET", {}, { { "Mcp-Session-Id", session } }), nullptr).status, 405);
	BOOST_CHECK(sink->text().empty());
}

BOOST_FIXTURE_TEST_CASE(EventStream_ReceivesNotifications, endpoint_fixture)
{
	const std::string session = initialize();
	const auto sink = std::make_shared<fake_sink>();
	const auto head = send(make("GET", {}, { { "Mcp-Session-Id", session } }), sink);
	BOOST_CHECK(head.stream);
	BOOST_CHECK_EQUAL(head.status, 200);
	BOOST_CHECK_EQUAL(header_of(head, "Content-Type"), "text/event-stream");
	BOOST_CHECK_EQUAL(sink->text(), ": stream open\n\n");

	ep.notify_tools_changed();
	const auto messages = sink->messages();
	BOOST_REQUIRE_EQUAL(messages.size(), 1u);
	BOOST_CHECK_EQUAL(std::string(messages[0].as_object().at("method").as_string()), "notifications/tools/list_changed");
	BOOST_CHECK(!messages[0].as_object().contains("id"));
	BOOST_CHECK(!sink->closed());
}

BOOST_FIXTURE_TEST_CASE(EventStream_ResourceUpdatesGoToSubscribersOnly, endpoint_fixture)
{
	const std::string subscriber = initialize();
	const std::string bystander = initialize();
	const auto subscriber_sink = std::make_shared<fake_sink>();
	const auto bystander_sink = std::make_shared<fake_sink>();
	send(make("GET", {}, { { "Mcp-Session-Id", subscriber } }), subscriber_sink);
	send(make("GET", {}, { { "Mcp-Session-Id", bystander } }), bystander_sink);
	call(subscriber, "resources/subscribe", R"({"uri":"snk://log"})");

	ep.notify_resource_updated("snk://log");
	ep.notify_resource_updated("snk://other");

	const auto messages = subscriber_sink->messages();
	BOOST_REQUIRE_EQUAL(messages.size(), 1u);
	BOOST_CHECK_EQUAL(std::string(messages[0].as_object().at("method").as_string()), "notifications/resources/updated");
	BOOST_CHECK_EQUAL(std::string(messages[0].as_object().at("params").as_object().at("uri").as_string()), "snk://log");
	BOOST_CHECK(bystander_sink->messages().empty());

	call(subscriber, "resources/unsubscribe", R"({"uri":"snk://log"})");
	ep.notify_resource_updated("snk://log");
	BOOST_CHECK_EQUAL(subscriber_sink->messages().size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(EventStream_ANewStreamReplacesTheOldOne, endpoint_fixture)
{
	const std::string session = initialize();
	const auto first = std::make_shared<fake_sink>();
	const auto second = std::make_shared<fake_sink>();
	send(make("GET", {}, { { "Mcp-Session-Id", session } }), first);
	send(make("GET", {}, { { "Mcp-Session-Id", session } }), second);
	BOOST_CHECK(first->closed());
	BOOST_CHECK(!second->closed());
	ep.notify_tools_changed();
	BOOST_CHECK(first->messages().empty());
	BOOST_CHECK_EQUAL(second->messages().size(), 1u);
}

BOOST_FIXTURE_TEST_CASE(NotificationsWithoutAStream_AreDropped, endpoint_fixture)
{
	initialize();
	ep.notify_tools_changed();
	ep.notify_resource_updated("snk://log");
	BOOST_CHECK_EQUAL(ep.session_count(), 1u);
}

BOOST_FIXTURE_TEST_CASE(Delete_EndsTheSessionAndItsStream, endpoint_fixture)
{
	const std::string session = initialize();
	const auto sink = std::make_shared<fake_sink>();
	send(make("GET", {}, { { "Mcp-Session-Id", session } }), sink);

	BOOST_CHECK_EQUAL(send(make("DELETE", {}, { { "Mcp-Session-Id", session } })).status, 200);
	BOOST_CHECK(sink->closed());
	BOOST_CHECK_EQUAL(ep.session_count(), 0u);
	BOOST_CHECK_EQUAL(send(make("POST", rpc("ping", {}), { { "Mcp-Session-Id", session } })).status, 404);
	BOOST_CHECK_EQUAL(send(make("DELETE", {}, { { "Mcp-Session-Id", session } })).status, 404);
	BOOST_CHECK_EQUAL(send(make("DELETE")).status, 400);
}

BOOST_AUTO_TEST_CASE(Sessions_TheLeastRecentlyUsedGoesWhenTheLimitIsReached)
{
	mcp::server_config cfg = endpoint_fixture::default_config();
	cfg.max_sessions = 2;
	endpoint_fixture f(cfg);
	const std::string oldest = f.initialize();
	const std::string used = f.initialize();
	f.call(used, "ping");
	const std::string newest = f.initialize();

	BOOST_CHECK_EQUAL(f.ep.session_count(), 2u);
	BOOST_CHECK_EQUAL(f.send(f.make("POST", endpoint_fixture::rpc("ping", {}), { { "Mcp-Session-Id", oldest } })).status, 404);
	BOOST_CHECK_EQUAL(f.send(f.make("POST", endpoint_fixture::rpc("ping", {}), { { "Mcp-Session-Id", used } })).status, 200);
	BOOST_CHECK_EQUAL(f.send(f.make("POST", endpoint_fixture::rpc("ping", {}), { { "Mcp-Session-Id", newest } })).status, 200);
}

BOOST_FIXTURE_TEST_CASE(CloseAll_EndsEverySession, endpoint_fixture)
{
	const std::string a = initialize();
	initialize();
	const auto sink = std::make_shared<fake_sink>();
	send(make("GET", {}, { { "Mcp-Session-Id", a } }), sink);
	ep.close_all();
	BOOST_CHECK(sink->closed());
	BOOST_CHECK_EQUAL(ep.session_count(), 0u);
	BOOST_CHECK_EQUAL(send(make("POST", rpc("ping", {}), { { "Mcp-Session-Id", a } })).status, 404);
}

BOOST_AUTO_TEST_CASE(AnswerAfterTheEndpointIsGone_IsDropped)
{
	auto host = std::make_unique<fake_host>();
	host->hold_calls = true;
	mcp::tool_info tool;
	tool.name = "t";
	host->tools.push_back(tool);
	mcp::server_config cfg = endpoint_fixture::default_config();
	response_box box;
	{
		mcp::endpoint ep(*host, cfg);
		ep.set_port(bound_port);
		endpoint_fixture helper;
		// A session made on this endpoint through the same request shapes.
		mcp::http_request init = helper.make("POST", endpoint_fixture::rpc("initialize", R"({"protocolVersion":"2025-06-18"})"));
		response_box init_box;
		ep.handle(std::move(init), init_box.reply(), nullptr);
		const std::string session = header_of(init_box.responses.front(), "Mcp-Session-Id");
		ep.handle(helper.make("POST", endpoint_fixture::rpc("tools/call", R"({"name":"t"})"), { { "Mcp-Session-Id", session } }), box.reply(), nullptr);
	}
	BOOST_REQUIRE_EQUAL(host->held.size(), 1u);
	host->held[0].done(mcp::call_result{}); // the server is gone: nothing is delivered, nothing crashes
	BOOST_CHECK_EQUAL(box.count(), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
