#include <boost/test/unit_test.hpp>
#include "mcp_test_support.hpp"
#include <snk/mcp/mcp_endpoint.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// snk_mcp_server without sockets or the engine: HTTP requests go into mcp::endpoint, a fake host answers, the
// responses and the event stream are read back. Covers the protocol, the sessions, the token and the Host/Origin checks.

namespace {

namespace json = boost::json;
using mcp_test::fake_host;
using mcp_test::json_of;

constexpr std::uint16_t test_port = 4711;

std::string text_of(const json::value& value)
{
	const json::string& text = value.as_string();
	return std::string(text.data(), text.size());
}

mcp::endpoint_config make_config()
{
	mcp::endpoint_config config;
	config.token = mcp_test::test_token;
	config.info.name = "test-server";
	config.info.version = "9";
	config.info.snk_api = "3";
	return config;
}

mcp::http_request make_request(std::string method, std::string body = {})
{
	mcp::http_request request;
	request.method = std::move(method);
	request.target = "/mcp";
	request.body = std::move(body);
	request.headers.push_back({ "host", "127.0.0.1:" + std::to_string(test_port) });
	request.headers.push_back({ "authorization", std::string("Bearer ") + mcp_test::test_token });
	request.headers.push_back({ "content-type", "application/json" });
	return request;
}

void set_header(mcp::http_request& request, const std::string& name, const std::string& value)
{
	for (mcp::http_header& h : request.headers) {
		if (h.name == name) {
			h.value = value;
			return;
		}
	}
	request.headers.push_back({ name, value });
}

void remove_header(mcp::http_request& request, const std::string& name)
{
	std::erase_if(request.headers, [&name](const mcp::http_header& h) { return h.name == name; });
}

std::string rpc(const std::string& id_json, const std::string& method, const std::string& params_json = "{}")
{
	return R"({"jsonrpc":"2.0","id":)" + id_json + R"(,"method":")" + method + R"(","params":)" + params_json + "}";
}

struct endpoint_fixture
{
	fake_host host;
	mcp::endpoint ep;

	endpoint_fixture()
		: ep(host, make_config())
	{
		ep.set_port(test_port);
	}

	using pending_response = std::shared_ptr<std::optional<mcp::http_response>>;

	// Empty until the endpoint answers, which may be after handle() returned.
	pending_response try_send(const mcp::http_request& request)
	{
		auto captured = std::make_shared<std::optional<mcp::http_response>>();
		ep.handle(request, [captured](mcp::http_response response) { *captured = std::move(response); });
		return captured;
	}

	mcp::http_response send(const mcp::http_request& request)
	{
		const pending_response response = try_send(request);
		BOOST_REQUIRE_MESSAGE(response->has_value(), "the endpoint did not answer");
		return std::move(**response);
	}

	mcp::http_request session_request(const std::string& session, std::string body, std::string method = "POST")
	{
		mcp::http_request request = make_request(std::move(method), std::move(body));
		set_header(request, "mcp-session-id", session);
		return request;
	}

	std::string initialize(const std::string& version = "2025-06-18")
	{
		const mcp::http_response response = send(make_request("POST",
			rpc("1", "initialize", R"({"protocolVersion":")" + version + R"(","capabilities":{},"clientInfo":{"name":"t","version":"1"}})")));
		BOOST_REQUIRE_EQUAL(response.status, 200);
		const std::string* id = nullptr;
		for (const mcp::http_header& h : response.headers) {
			if (h.name == "Mcp-Session-Id") {
				id = &h.value;
			}
		}
		BOOST_REQUIRE(id != nullptr);
		return *id;
	}

	// The JSON-RPC result of a call in an established session.
	json::value call(const std::string& session, const std::string& method, const std::string& params = "{}")
	{
		const mcp::http_response response = send(session_request(session, rpc("7", method, params)));
		BOOST_REQUIRE_EQUAL(response.status, 200);
		return json_of(response.body);
	}
};

// The data of the "event: message" frames, parsed.
std::vector<json::value> events_of(const std::vector<std::string>& frames)
{
	std::vector<json::value> out;
	for (const std::string& frame : frames) {
		const std::string prefix = "event: message\ndata: ";
		if (frame.rfind(prefix, 0) == 0) {
			out.push_back(json_of(std::string_view(frame).substr(prefix.size(), frame.size() - prefix.size() - 2)));
		}
	}
	return out;
}

}

BOOST_FIXTURE_TEST_SUITE(McpEndpointGuardTests, endpoint_fixture)

BOOST_AUTO_TEST_CASE(NoToken_Is401AndTheConnectionCloses)
{
	mcp::http_request request = make_request("POST", rpc("1", "ping"));
	remove_header(request, "authorization");
	const mcp::http_response response = send(request);
	BOOST_TEST(response.status == 401);
	BOOST_TEST(response.close);
	bool challenge = false;
	for (const mcp::http_header& h : response.headers) {
		challenge = challenge || h.name == "WWW-Authenticate";
	}
	BOOST_TEST(challenge);
	BOOST_TEST(host.calls().empty());
}

BOOST_AUTO_TEST_CASE(WrongToken_Is401)
{
	const std::vector<std::string> wrong = { "Bearer nope", "Bearer ", "Bearer", "Basic abc", std::string("Bearer ") + mcp_test::test_token + "x" };
	for (const std::string& value : wrong) {
		mcp::http_request request = make_request("POST", rpc("1", "ping"));
		set_header(request, "authorization", value);
		BOOST_TEST_CONTEXT("authorization: " << value) {
			BOOST_TEST(send(request).status == 401);
		}
	}
}

BOOST_AUTO_TEST_CASE(SchemeIsCaseInsensitive)
{
	mcp::http_request request = make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})"));
	set_header(request, "authorization", std::string("bearer ") + mcp_test::test_token);
	BOOST_TEST(send(request).status == 200);
}

BOOST_AUTO_TEST_CASE(EmptyConfiguredToken_RefusesEverything)
{
	mcp::endpoint_config config = make_config();
	config.token.clear();
	mcp::endpoint closed(host, config);
	closed.set_port(test_port);
	mcp::http_request request = make_request("POST", rpc("1", "ping"));
	set_header(request, "authorization", "Bearer ");
	std::optional<mcp::http_response> response;
	closed.handle(request, [&response](mcp::http_response r) { response = std::move(r); });
	BOOST_REQUIRE(response.has_value());
	BOOST_TEST(response->status == 401);
}

BOOST_AUTO_TEST_CASE(HostHeader_MustNameTheLoopbackAndThePort)
{
	const std::vector<std::string> refused = { "evil.example:4711", "127.0.0.1:1", "127.0.0.1", "10.0.0.5:4711", "localhost.evil.com:4711" };
	for (const std::string& value : refused) {
		mcp::http_request request = make_request("POST", rpc("1", "ping"));
		set_header(request, "host", value);
		BOOST_TEST_CONTEXT("host: " << value) {
			const mcp::http_response response = send(request);
			BOOST_TEST(response.status == 403);
			BOOST_TEST(response.close);
		}
	}
	mcp::http_request by_name = make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})"));
	set_header(by_name, "host", "LocalHost:4711");
	BOOST_TEST(send(by_name).status == 200);

	mcp::http_request missing = make_request("POST", rpc("1", "ping"));
	remove_header(missing, "host");
	BOOST_TEST(send(missing).status == 403);
}

BOOST_AUTO_TEST_CASE(HostIsCheckedBeforeTheToken)
{
	mcp::http_request request = make_request("POST", rpc("1", "ping"));
	set_header(request, "host", "evil.example:4711");
	remove_header(request, "authorization");
	BOOST_TEST(send(request).status == 403);
}

BOOST_AUTO_TEST_CASE(Origin_IsRefusedUnlessListed)
{
	mcp::http_request browser = make_request("POST", rpc("1", "ping"));
	set_header(browser, "origin", "http://evil.example");
	BOOST_TEST(send(browser).status == 403);

	mcp::endpoint_config config = make_config();
	config.allowed_origins = { "http://localhost:6274" };
	mcp::endpoint allowing(host, config);
	allowing.set_port(test_port);
	mcp::http_request inspector = make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})"));
	set_header(inspector, "origin", "http://localhost:6274");
	std::optional<mcp::http_response> response;
	allowing.handle(inspector, [&response](mcp::http_response r) { response = std::move(r); });
	BOOST_REQUIRE(response.has_value());
	BOOST_TEST(response->status == 200);

	set_header(inspector, "origin", "http://localhost:6275");
	response.reset();
	allowing.handle(inspector, [&response](mcp::http_response r) { response = std::move(r); });
	BOOST_REQUIRE(response.has_value());
	BOOST_TEST(response->status == 403);
}

BOOST_AUTO_TEST_CASE(PathAndMethod_AreChecked)
{
	mcp::http_request elsewhere = make_request("POST", rpc("1", "ping"));
	elsewhere.target = "/other";
	BOOST_TEST(send(elsewhere).status == 404);

	mcp::http_request with_query = make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})"));
	with_query.target = "/mcp?x=1";
	BOOST_TEST(send(with_query).status == 200);

	const mcp::http_response put = send(make_request("PUT", "{}"));
	BOOST_TEST(put.status == 405);
}

BOOST_AUTO_TEST_CASE(Post_NeedsJsonAndValidJson)
{
	mcp::http_request text = make_request("POST", "hello");
	set_header(text, "content-type", "text/plain");
	BOOST_TEST(send(text).status == 415);

	const mcp::http_response broken = send(make_request("POST", "{not json"));
	BOOST_TEST(broken.status == 400);
	BOOST_TEST(json_of(broken.body).at("error").at("code").as_int64() == -32700);

	const mcp::http_response batch = send(make_request("POST", "[]"));
	BOOST_TEST(batch.status == 400);
	BOOST_TEST(json_of(batch.body).at("error").at("code").as_int64() == -32600);

	BOOST_TEST(send(make_request("POST", "42")).status == 400);
	BOOST_TEST(send(make_request("POST", R"({"jsonrpc":"2.0"})")).status == 400);
	BOOST_TEST(send(make_request("POST", R"({"jsonrpc":"2.0","id":null,"method":"ping"})")).status == 400);
	BOOST_TEST(send(make_request("POST", R"({"jsonrpc":"2.0","id":1,"method":5})")).status == 400);
}

BOOST_AUTO_TEST_CASE(DeepJson_IsRefusedNotCrashed)
{
	const std::string deep = std::string(500, '[') + std::string(500, ']');
	BOOST_TEST(send(make_request("POST", deep)).status == 400);
}

BOOST_AUTO_TEST_CASE(ProtocolVersionHeader_MustBeSupported)
{
	mcp::http_request bad = make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})"));
	set_header(bad, "mcp-protocol-version", "1999-01-01");
	BOOST_TEST(send(bad).status == 400);

	mcp::http_request good = make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})"));
	set_header(good, "mcp-protocol-version", "2025-03-26");
	BOOST_TEST(send(good).status == 200);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(McpEndpointProtocolTests, endpoint_fixture)

BOOST_AUTO_TEST_CASE(Initialize_CreatesASessionAndNegotiatesTheVersion)
{
	const mcp::http_response response = send(make_request("POST",
		rpc("\"abc\"", "initialize", R"({"protocolVersion":"2025-03-26","capabilities":{}})")));
	BOOST_REQUIRE_EQUAL(response.status, 200);
	const json::value body = json_of(response.body);
	BOOST_TEST(text_of(body.at("id")) == "abc");
	const json::object& result = body.at("result").as_object();
	BOOST_TEST(text_of(result.at("protocolVersion")) == "2025-03-26");
	BOOST_TEST(text_of(result.at("serverInfo").at("name")) == "test-server");
	BOOST_TEST(text_of(result.at("serverInfo").at("version")) == "9");
	BOOST_TEST(text_of(result.at("serverInfo").at("snk_api")) == "3");
	BOOST_TEST(result.at("capabilities").at("tools").at("listChanged").as_bool());
	BOOST_TEST(result.at("capabilities").at("resources").at("subscribe").as_bool());
	BOOST_TEST(ep.session_count() == 1u);

	bool has_session_header = false;
	for (const mcp::http_header& h : response.headers) {
		if (h.name == "Mcp-Session-Id") {
			has_session_header = h.value.size() == 32;
		}
	}
	BOOST_TEST(has_session_header);
}

BOOST_AUTO_TEST_CASE(Initialize_AnUnknownVersionGetsTheLatestOurs)
{
	const mcp::http_response response = send(make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2099-01-01"})")));
	BOOST_TEST(text_of(json_of(response.body).at("result").at("protocolVersion")) == "2025-06-18");
}

BOOST_AUTO_TEST_CASE(Initialize_NeedsAProtocolVersionAndCreatesNoSession)
{
	const mcp::http_response response = send(make_request("POST", rpc("1", "initialize", "{}")));
	BOOST_TEST(json_of(response.body).at("error").at("code").as_int64() == -32602);
	BOOST_TEST(ep.session_count() == 0u);
}

BOOST_AUTO_TEST_CASE(SessionIds_AreUnique)
{
	const std::string first = initialize();
	const std::string second = initialize();
	BOOST_TEST(first != second);
	BOOST_TEST(ep.session_count() == 2u);
}

BOOST_AUTO_TEST_CASE(NoSessionHeader_Is400AndAnUnknownSessionIs404)
{
	BOOST_TEST(send(make_request("POST", rpc("1", "ping"))).status == 400);
	BOOST_TEST(send(session_request("ffffffffffffffffffffffffffffffff", rpc("1", "ping"))).status == 404);
}

BOOST_AUTO_TEST_CASE(Notifications_AreAcceptedWithoutABody)
{
	const std::string session = initialize();
	const mcp::http_response response = send(session_request(session, R"({"jsonrpc":"2.0","method":"notifications/initialized"})"));
	BOOST_TEST(response.status == 202);
	BOOST_TEST(response.body.empty());

	const mcp::http_response client_reply = send(session_request(session, R"({"jsonrpc":"2.0","id":3,"result":{}})"));
	BOOST_TEST(client_reply.status == 202);
}

BOOST_AUTO_TEST_CASE(Ping_AnswersAnEmptyResult)
{
	const std::string session = initialize();
	const json::value body = call(session, "ping");
	BOOST_TEST(body.at("id").as_int64() == 7);
	BOOST_TEST(body.at("result").as_object().empty());
}

BOOST_AUTO_TEST_CASE(UnknownMethod_IsMethodNotFound)
{
	const std::string session = initialize();
	BOOST_TEST(call(session, "prompts/list").at("error").at("code").as_int64() == -32601);
}

BOOST_AUTO_TEST_CASE(ToolsList_ReportsSchemasAndAnnotations)
{
	mcp::tool_info with_output = mcp_test::make_tool("viewport_capture");
	with_output.output_schema = json_of(R"({"type":"object"})");
	mcp::tool_info bare;
	bare.name = "bare";
	bare.description = "No schema";
	host.set_tools({ with_output, bare });
	const std::string session = initialize();

	const json::value body = call(session, "tools/list");
	const json::array& tools = body.at("result").at("tools").as_array();
	BOOST_REQUIRE_EQUAL(tools.size(), 2u);
	BOOST_TEST(text_of(tools[0].at("name")) == "viewport_capture");
	BOOST_TEST(text_of(tools[0].at("inputSchema").at("type")) == "object");
	BOOST_TEST(text_of(tools[0].at("outputSchema").at("type")) == "object");
	BOOST_TEST(tools[0].at("annotations").at("readOnlyHint").as_bool());
	BOOST_TEST(!tools[0].at("annotations").at("destructiveHint").as_bool());
	BOOST_TEST(text_of(tools[1].at("inputSchema").at("type")) == "object");
	BOOST_TEST(!tools[1].as_object().contains("outputSchema"));
	BOOST_TEST(!tools[1].at("annotations").at("readOnlyHint").as_bool());
}

BOOST_AUTO_TEST_CASE(ToolsCall_PassesTheNameArgumentsAndSession)
{
	mcp::call_result answer;
	answer.structured = json_of(R"({"sum":5})");
	host.set_next_result(answer);
	const std::string session = initialize();

	const json::value body = call(session, "tools/call", R"({"name":"math_sum","arguments":{"a":2,"b":3}})");
	const auto calls = host.calls();
	BOOST_REQUIRE_EQUAL(calls.size(), 1u);
	BOOST_TEST(calls[0].name == "math_sum");
	BOOST_TEST(calls[0].session_id == session);
	BOOST_TEST(json::serialize(calls[0].args) == R"({"a":2,"b":3})");

	const json::object& result = body.at("result").as_object();
	BOOST_TEST(!result.at("isError").as_bool());
	BOOST_TEST(result.at("structuredContent").at("sum").as_int64() == 5);
	// A client without structuredContent still gets the data.
	BOOST_REQUIRE_EQUAL(result.at("content").as_array().size(), 1u);
	BOOST_TEST(text_of(result.at("content").as_array()[0].at("type")) == "text");
	BOOST_TEST(json_of(result.at("content").as_array()[0].at("text").as_string().c_str()).at("sum").as_int64() == 5);
}

BOOST_AUTO_TEST_CASE(ToolsCall_MapsTextAndImageContentAndErrors)
{
	mcp::call_result answer;
	answer.is_error = true;
	answer.content.push_back(mcp::content_block::make_text("denied"));
	answer.content.push_back(mcp::content_block::make_image("QUJD", "image/png"));
	host.set_next_result(answer);
	const std::string session = initialize();

	const json::value body = call(session, "tools/call", R"({"name":"x"})");
	const json::object& result = body.at("result").as_object();
	BOOST_TEST(result.at("isError").as_bool());
	BOOST_TEST(!result.contains("structuredContent"));
	const json::array& content = result.at("content").as_array();
	BOOST_REQUIRE_EQUAL(content.size(), 2u);
	BOOST_TEST(text_of(content[0].at("text")) == "denied");
	BOOST_TEST(text_of(content[1].at("type")) == "image");
	BOOST_TEST(text_of(content[1].at("data")) == "QUJD");
	BOOST_TEST(text_of(content[1].at("mimeType")) == "image/png");
	// No arguments given: the host gets an empty object.
	BOOST_TEST(json::serialize(host.calls().at(0).args) == "{}");
}

BOOST_AUTO_TEST_CASE(ToolsCall_ChecksItsParameters)
{
	const std::string session = initialize();
	BOOST_TEST(call(session, "tools/call", "{}").at("error").at("code").as_int64() == -32602);
	BOOST_TEST(call(session, "tools/call", R"({"name":5})").at("error").at("code").as_int64() == -32602);
	BOOST_TEST(call(session, "tools/call", R"({"name":"x","arguments":[1]})").at("error").at("code").as_int64() == -32602);
	BOOST_TEST(host.calls().empty());
}

BOOST_AUTO_TEST_CASE(ToolsCall_AnswersWhenTheHostDoes)
{
	host.set_defer_calls(true);
	const std::string session = initialize();

	const pending_response response = try_send(session_request(session, rpc("9", "tools/call", R"({"name":"slow"})")));
	BOOST_TEST(!response->has_value());

	// A second request is answered meanwhile: a slow tool does not block the endpoint.
	BOOST_TEST(call(session, "ping").at("result").as_object().empty());

	mcp::call_result answer;
	answer.content.push_back(mcp::content_block::make_text("done"));
	BOOST_TEST(host.answer_deferred(answer));
	BOOST_REQUIRE(response->has_value());
	const json::value answered = json_of((*response)->body);
	BOOST_TEST(text_of(answered.at("result").at("content").as_array()[0].at("text")) == "done");
	BOOST_TEST(!host.answer_deferred(answer));
}

BOOST_AUTO_TEST_CASE(ToolsCall_ARespondCallsBackExactlyOnce)
{
	host.set_defer_calls(true);
	const std::string session = initialize();
	int answers = 0;
	json::value last;
	ep.handle(session_request(session, rpc("9", "tools/call", R"({"name":"slow"})")), [&](mcp::http_response r) {
		++answers;
		last = json_of(r.body);
	});
	BOOST_TEST(answers == 0);

	mcp::call_result answer;
	answer.content.push_back(mcp::content_block::make_text("first"));
	BOOST_REQUIRE(host.answer_deferred(answer));
	BOOST_TEST(answers == 1);
	BOOST_TEST(last.at("id").as_int64() == 9);
	BOOST_TEST(text_of(last.at("result").at("content").as_array()[0].at("text")) == "first");
}

BOOST_AUTO_TEST_CASE(HostThatThrows_IsAnInternalError)
{
	const std::string session = initialize();
	host.set_throw_on_list(true);
	const mcp::http_response response = send(session_request(session, rpc("1", "tools/list")));
	BOOST_TEST(response.status == 500);

	host.set_throw_on_call(true);
	BOOST_TEST(send(session_request(session, rpc("2", "tools/call", R"({"name":"x"})"))).status == 500);
	BOOST_TEST(!host.logs().empty());
}

BOOST_AUTO_TEST_CASE(Resources_ListAndRead)
{
	mcp::resource_info info;
	info.uri = "snk://log";
	info.name = "Log";
	info.mime_type = "application/json";
	host.set_resources({ info });

	mcp::resource_result text;
	text.found = true;
	mcp::resource_content text_content;
	text_content.uri = "snk://log";
	text_content.mime_type = "application/json";
	text_content.text = "{}";
	text.contents.push_back(text_content);
	host.set_resource_result("snk://log", text);

	mcp::resource_result blob;
	blob.found = true;
	mcp::resource_content blob_content;
	blob_content.uri = "snk://viewport/scene.png";
	blob_content.mime_type = "image/png";
	blob_content.blob = "QUJD";
	blob_content.is_blob = true;
	blob.contents.push_back(blob_content);
	host.set_resource_result("snk://viewport/scene.png", blob);

	const std::string session = initialize();
	const json::value listed = call(session, "resources/list");
	const json::array& resources = listed.at("result").at("resources").as_array();
	BOOST_REQUIRE_EQUAL(resources.size(), 1u);
	BOOST_TEST(text_of(resources[0].at("uri")) == "snk://log");
	BOOST_TEST(text_of(resources[0].at("mimeType")) == "application/json");
	BOOST_TEST(!resources[0].as_object().contains("description"));

	const json::value read_text = call(session, "resources/read", R"({"uri":"snk://log"})");
	BOOST_TEST(text_of(read_text.at("result").at("contents").as_array()[0].at("text")) == "{}");

	const json::value read_blob = call(session, "resources/read", R"({"uri":"snk://viewport/scene.png"})");
	const json::object& entry = read_blob.at("result").at("contents").as_array()[0].as_object();
	BOOST_TEST(text_of(entry.at("blob")) == "QUJD");
	BOOST_TEST(!entry.contains("text"));

	const json::value missing = call(session, "resources/read", R"({"uri":"snk://nope"})");
	BOOST_TEST(missing.at("error").at("code").as_int64() == -32002);
	BOOST_TEST(call(session, "resources/read", "{}").at("error").at("code").as_int64() == -32602);
	BOOST_TEST(call(session, "resources/templates/list").at("result").at("resourceTemplates").as_array().empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(McpEndpointStreamTests, endpoint_fixture)

namespace {

mcp::http_request get_stream(endpoint_fixture& fixture, const std::string& session)
{
	mcp::http_request request = fixture.session_request(session, {}, "GET");
	set_header(request, "accept", "text/event-stream");
	return request;
}

}

BOOST_AUTO_TEST_CASE(Get_OpensAStreamThatSaysSo)
{
	const std::string session = initialize();
	const mcp::http_response response = send(get_stream(*this, session));
	BOOST_REQUIRE_EQUAL(response.status, 200);
	BOOST_REQUIRE(response.stream != nullptr);
	BOOST_TEST((response.stream->take() == std::vector<std::string>{ ": stream open\n\n" }));
}

BOOST_AUTO_TEST_CASE(Get_NeedsTheEventStreamAcceptAndASession)
{
	const std::string session = initialize();
	mcp::http_request no_accept = session_request(session, {}, "GET");
	BOOST_TEST(send(no_accept).status == 406);
	BOOST_TEST(send(get_stream(*this, "ffffffffffffffffffffffffffffffff")).status == 404);
}

BOOST_AUTO_TEST_CASE(ResourceUpdates_GoOnlyToSubscribers)
{
	const std::string subscriber = initialize();
	const std::string bystander = initialize();
	const auto subscriber_stream = send(get_stream(*this, subscriber)).stream;
	const auto bystander_stream = send(get_stream(*this, bystander)).stream;
	subscriber_stream->take();
	bystander_stream->take();

	BOOST_TEST(call(subscriber, "resources/subscribe", R"({"uri":"snk://log"})").at("result").as_object().empty());
	ep.notify_resource_updated("snk://log");
	ep.notify_resource_updated("snk://other");

	const std::vector<json::value> events = events_of(subscriber_stream->take());
	BOOST_REQUIRE_EQUAL(events.size(), 1u);
	BOOST_TEST(text_of(events[0].at("method")) == "notifications/resources/updated");
	BOOST_TEST(text_of(events[0].at("params").at("uri")) == "snk://log");
	BOOST_TEST(!events[0].as_object().contains("id"));
	BOOST_TEST(bystander_stream->take().empty());

	BOOST_TEST(call(subscriber, "resources/unsubscribe", R"({"uri":"snk://log"})").at("result").as_object().empty());
	ep.notify_resource_updated("snk://log");
	BOOST_TEST(subscriber_stream->take().empty());
}

BOOST_AUTO_TEST_CASE(ToolsChanged_GoesToEveryStream)
{
	const std::string a = initialize();
	const std::string b = initialize();
	const std::string no_stream = initialize();
	const auto stream_a = send(get_stream(*this, a)).stream;
	const auto stream_b = send(get_stream(*this, b)).stream;
	stream_a->take();
	stream_b->take();

	ep.notify_tools_changed();
	for (const auto& stream : { stream_a, stream_b }) {
		const std::vector<json::value> events = events_of(stream->take());
		BOOST_REQUIRE_EQUAL(events.size(), 1u);
		BOOST_TEST(text_of(events[0].at("method")) == "notifications/tools/list_changed");
	}
	(void)no_stream;
}

BOOST_AUTO_TEST_CASE(Progress_GoesToTheStreamWithTheToken)
{
	host.set_defer_calls(true);
	const std::string session = initialize();
	const auto stream = send(get_stream(*this, session)).stream;
	stream->take();

	ep.handle(session_request(session, rpc("5", "tools/call", R"({"name":"long","_meta":{"progressToken":"tok-1"}})")), [](mcp::http_response) {});
	ep.handle(session_request(session, rpc("6", "tools/call", R"({"name":"plain"})")), [](mcp::http_response) {});
	const auto calls = host.calls();
	BOOST_REQUIRE_EQUAL(calls.size(), 2u);

	calls[0].progress(2.0, 10.0, "working");
	calls[1].progress(1.0, 0.0, "nobody listens for a token");

	const std::vector<json::value> events = events_of(stream->take());
	BOOST_REQUIRE_EQUAL(events.size(), 1u);
	BOOST_TEST(text_of(events[0].at("method")) == "notifications/progress");
	const json::object& params = events[0].at("params").as_object();
	BOOST_TEST(text_of(params.at("progressToken")) == "tok-1");
	BOOST_TEST(params.at("progress").to_number<double>() == 2.0);
	BOOST_TEST(params.at("total").to_number<double>() == 10.0);
	BOOST_TEST(text_of(params.at("message")) == "working");
}

BOOST_AUTO_TEST_CASE(Progress_AfterTheSessionEndedIsIgnored)
{
	host.set_defer_calls(true);
	const std::string session = initialize();
	const auto stream = send(get_stream(*this, session)).stream;
	stream->take();
	ep.handle(session_request(session, rpc("5", "tools/call", R"({"name":"long","_meta":{"progressToken":7}})")), [](mcp::http_response) {});
	const auto calls = host.calls();
	BOOST_REQUIRE_EQUAL(calls.size(), 1u);

	BOOST_TEST(send(session_request(session, {}, "DELETE")).status == 200);
	BOOST_TEST(stream->closed());
	calls[0].progress(1.0, 2.0, "late");
	BOOST_TEST(stream->take().empty());
}

BOOST_AUTO_TEST_CASE(SecondStream_ReplacesTheFirst)
{
	const std::string session = initialize();
	const auto first = send(get_stream(*this, session)).stream;
	const auto second = send(get_stream(*this, session)).stream;
	BOOST_TEST(first->closed());
	BOOST_TEST(!second->closed());
	ep.notify_tools_changed();
	BOOST_TEST(events_of(second->take()).size() == 1u);
}

BOOST_AUTO_TEST_CASE(Delete_EndsTheSession)
{
	const std::string session = initialize();
	const auto stream = send(get_stream(*this, session)).stream;
	BOOST_TEST(send(session_request(session, {}, "DELETE")).status == 200);
	BOOST_TEST(stream->closed());
	BOOST_TEST(ep.session_count() == 0u);
	BOOST_TEST(send(session_request(session, rpc("1", "ping"))).status == 404);
	BOOST_TEST(send(session_request(session, {}, "DELETE")).status == 404);
}

BOOST_AUTO_TEST_CASE(CloseAll_ClosesTheStreamsAndForgetsTheSessions)
{
	const std::string session = initialize();
	const auto stream = send(get_stream(*this, session)).stream;
	ep.close_all();
	BOOST_TEST(stream->closed());
	BOOST_TEST(ep.session_count() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(McpEndpointLimitTests)

BOOST_AUTO_TEST_CASE(TooManySessions_TheOldestMakesRoom)
{
	fake_host host;
	mcp::endpoint_config config = make_config();
	config.max_sessions = 2;
	mcp::endpoint ep(host, config);
	ep.set_port(test_port);

	auto open = [&]() -> std::string {
		std::optional<mcp::http_response> response;
		ep.handle(make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})")),
			[&response](mcp::http_response r) { response = std::move(r); });
		for (const mcp::http_header& h : response->headers) {
			if (h.name == "Mcp-Session-Id") {
				return h.value;
			}
		}
		return {};
	};
	auto status_of = [&](const std::string& session) {
		mcp::http_request request = make_request("POST", rpc("1", "ping"));
		set_header(request, "mcp-session-id", session);
		int status = 0;
		ep.handle(request, [&status](mcp::http_response r) { status = r.status; });
		return status;
	};

	const std::string first = open();
	const std::string second = open();
	const std::string third = open();
	BOOST_TEST(ep.session_count() == 2u);
	BOOST_TEST(status_of(first) == 404);
	BOOST_TEST(status_of(second) == 200);
	BOOST_TEST(status_of(third) == 200);
}

BOOST_AUTO_TEST_CASE(TooManySubscriptions_AreRefused)
{
	fake_host host;
	mcp::endpoint_config config = make_config();
	config.max_subscriptions_per_session = 2;
	mcp::endpoint ep(host, config);
	ep.set_port(test_port);

	std::string session;
	{
		std::optional<mcp::http_response> response;
		ep.handle(make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})")),
			[&response](mcp::http_response r) { response = std::move(r); });
		for (const mcp::http_header& h : response->headers) {
			if (h.name == "Mcp-Session-Id") {
				session = h.value;
			}
		}
	}
	auto subscribe = [&](const std::string& uri) {
		mcp::http_request request = make_request("POST", rpc("1", "resources/subscribe", R"({"uri":")" + uri + R"("})"));
		set_header(request, "mcp-session-id", session);
		json::value body;
		ep.handle(request, [&body](mcp::http_response r) { body = json_of(r.body); });
		return body;
	};
	BOOST_TEST(subscribe("snk://a").as_object().contains("result"));
	BOOST_TEST(subscribe("snk://b").as_object().contains("result"));
	BOOST_TEST(subscribe("snk://a").as_object().contains("result"));
	BOOST_TEST(subscribe("snk://c").as_object().contains("error"));
}

BOOST_AUTO_TEST_CASE(ManyThreads_UseTheEndpointAtOnce)
{
	fake_host host;
	host.set_tools({ mcp_test::make_tool("t") });
	mcp::endpoint_config config = make_config();
	config.max_sessions = 4;
	mcp::endpoint ep(host, config);
	ep.set_port(test_port);

	std::atomic<int> ok{ 0 };
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&] {
			for (int i = 0; i < 25; ++i) {
				ep.handle(make_request("POST", rpc("1", "initialize", R"({"protocolVersion":"2025-06-18"})")),
					[&](mcp::http_response r) {
						if (r.status == 200) {
							++ok;
						}
					});
				ep.notify_tools_changed();
			}
		});
	}
	for (std::thread& thread : threads) {
		thread.join();
	}
	BOOST_TEST(ok.load() == 100);
	BOOST_TEST(ep.session_count() == 4u);
}

BOOST_AUTO_TEST_SUITE_END()
