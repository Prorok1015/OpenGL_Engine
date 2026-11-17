#include <boost/test/unit_test.hpp>
#include <snk/mcp/mcp_http.h>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The HTTP layer of snk_mcp_server: the request parser, the response writer and the event channel. Pure data in,
// data out; no sockets.

namespace {

using state = mcp::http_request_parser::state;

std::string post_request(std::string_view body)
{
	std::string text = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:1\r\nContent-Type: application/json\r\nContent-Length: ";
	text += std::to_string(body.size());
	text += "\r\n\r\n";
	text += body;
	return text;
}

}

BOOST_AUTO_TEST_SUITE(McpHttpParserTests)

BOOST_AUTO_TEST_CASE(SimpleGet_ParsesTheHeadWithLowerCaseNames)
{
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed("GET /mcp?x=1 HTTP/1.1\r\nHost: localhost:8080\r\nAccept:  text/event-stream \r\n\r\n") == state::complete);

	const mcp::http_request request = parser.take();
	BOOST_TEST(request.method == "GET");
	BOOST_TEST(request.target == "/mcp?x=1");
	BOOST_TEST(request.body.empty());
	BOOST_TEST(request.keep_alive);
	BOOST_REQUIRE(request.header("host") != nullptr);
	BOOST_TEST(*request.header("host") == "localhost:8080");
	BOOST_REQUIRE(request.header("accept") != nullptr);
	BOOST_TEST(*request.header("accept") == "text/event-stream");
	BOOST_TEST((request.header("missing") == nullptr));
}

BOOST_AUTO_TEST_CASE(PostWithBody_NeedsTheWholeBody)
{
	const std::string text = post_request(R"({"a":1})");
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed(std::string_view(text).substr(0, text.size() - 3)) == state::need_more);
	BOOST_REQUIRE(parser.feed(std::string_view(text).substr(text.size() - 3)) == state::complete);
	const mcp::http_request request = parser.take();
	BOOST_TEST(request.method == "POST");
	BOOST_TEST(request.body == R"({"a":1})");
}

BOOST_AUTO_TEST_CASE(ByteByByte_GivesTheSameRequest)
{
	const std::string text = post_request(R"({"jsonrpc":"2.0"})");
	mcp::http_request_parser parser;
	state last = state::need_more;
	for (std::size_t i = 0; i < text.size(); ++i) {
		last = parser.feed(std::string_view(text).substr(i, 1));
		if (i + 1 < text.size()) {
			BOOST_REQUIRE(last == state::need_more);
		}
	}
	BOOST_REQUIRE(last == state::complete);
	BOOST_TEST(parser.take().body == R"({"jsonrpc":"2.0"})");
}

BOOST_AUTO_TEST_CASE(PipelinedRequests_AreParsedOneByOne)
{
	const std::string text = post_request("{}") + post_request("[]");
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed(text) == state::complete);
	BOOST_TEST(parser.take().body == "{}");
	BOOST_REQUIRE(parser.feed(std::string_view()) == state::complete);
	BOOST_TEST(parser.take().body == "[]");
	BOOST_TEST((parser.feed(std::string_view()) == state::need_more));
}

BOOST_AUTO_TEST_CASE(KeepAlive_FollowsTheVersionAndTheConnectionHeader)
{
	{
		mcp::http_request_parser parser;
		BOOST_REQUIRE(parser.feed("GET / HTTP/1.0\r\n\r\n") == state::complete);
		BOOST_TEST(!parser.take().keep_alive);
	}
	{
		mcp::http_request_parser parser;
		BOOST_REQUIRE(parser.feed("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n") == state::complete);
		BOOST_TEST(parser.take().keep_alive);
	}
	{
		mcp::http_request_parser parser;
		BOOST_REQUIRE(parser.feed("GET / HTTP/1.1\r\nConnection: close\r\n\r\n") == state::complete);
		BOOST_TEST(!parser.take().keep_alive);
	}
}

BOOST_AUTO_TEST_CASE(ExpectContinue_IsReportedBeforeTheBody)
{
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed("POST /mcp HTTP/1.1\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n") == state::need_more);
	BOOST_TEST(parser.expects_continue());
	BOOST_REQUIRE(parser.feed("{}") == state::complete);
	BOOST_TEST(parser.take().body == "{}");
	BOOST_TEST(!parser.expects_continue());
}

BOOST_AUTO_TEST_CASE(PendingRequest_IsReadableBeforeTheBodyArrives)
{
	mcp::http_request_parser parser;
	BOOST_TEST(!parser.buffered());
	BOOST_REQUIRE(parser.feed("POST /mcp HTTP/1.1\r\nExpect: 100-continue\r\nAuthorization: Bearer t\r\nContent-Length: 2\r\n\r\n") == state::need_more);
	BOOST_TEST(parser.buffered());
	BOOST_REQUIRE(parser.expects_continue());
	BOOST_REQUIRE(parser.pending_request().header("authorization") != nullptr);
	BOOST_TEST(*parser.pending_request().header("authorization") == "Bearer t");
}

BOOST_AUTO_TEST_CASE(HeadTooLarge_Is431)
{
	mcp::http_limits limits;
	limits.max_header_bytes = 64;
	mcp::http_request_parser parser(limits);
	BOOST_REQUIRE(parser.feed(std::string("GET / HTTP/1.1\r\nX-Long: ") + std::string(100, 'a')) == state::error);
	BOOST_TEST(parser.error_status() == 431);
	BOOST_TEST((parser.feed("\r\n\r\n") == state::error));
}

BOOST_AUTO_TEST_CASE(CompleteHeadOverTheLimit_Is431)
{
	mcp::http_limits limits;
	limits.max_header_bytes = 40;
	mcp::http_request_parser parser(limits);
	BOOST_REQUIRE(parser.feed(std::string("GET / HTTP/1.1\r\nX-Long: ") + std::string(60, 'a') + "\r\n\r\n") == state::error);
	BOOST_TEST(parser.error_status() == 431);
}

BOOST_AUTO_TEST_CASE(TooManyHeaders_Is431)
{
	mcp::http_limits limits;
	limits.max_headers = 2;
	mcp::http_request_parser parser(limits);
	BOOST_REQUIRE(parser.feed("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n") == state::error);
	BOOST_TEST(parser.error_status() == 431);
}

BOOST_AUTO_TEST_CASE(BodyOverTheLimit_Is413BeforeTheBodyArrives)
{
	mcp::http_limits limits;
	limits.max_body_bytes = 10;
	mcp::http_request_parser parser(limits);
	BOOST_REQUIRE(parser.feed("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n") == state::error);
	BOOST_TEST(parser.error_status() == 413);
}

BOOST_AUTO_TEST_CASE(Chunked_Is501)
{
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n") == state::error);
	BOOST_TEST(parser.error_status() == 501);
}

BOOST_AUTO_TEST_CASE(Malformed_Is400)
{
	const std::vector<std::string> bad = {
		"GARBAGE\r\n\r\n",
		"get / HTTP/1.1\r\n\r\n",
		"GET  HTTP/1.1\r\n\r\n",
		"GET noslash HTTP/1.1\r\n\r\n",
		"GET / FTP/1.1\r\n\r\n",
		"GET / HTTP/1.1\r\nNoColon\r\n\r\n",
		"GET / HTTP/1.1\r\n: value\r\n\r\n",
		"GET / HTTP/1.1\r\nBad Name: x\r\n\r\n",
		"GET / HTTP/1.1\r\nA: 1\r\n folded\r\n\r\n",
		"GET / HTTP/1.1\r\nA: 1\nB: 2\r\n\r\n",
		"POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n",
		"POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n",
		"POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n",
	};
	for (const std::string& text : bad) {
		mcp::http_request_parser parser;
		BOOST_TEST_CONTEXT("request: " << text) {
			BOOST_REQUIRE(parser.feed(text) == state::error);
			BOOST_TEST(parser.error_status() == 400);
			BOOST_TEST(!parser.error_message().empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(UnknownHttpVersion_Is505)
{
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed("GET / HTTP/2.0\r\n\r\n") == state::error);
	BOOST_TEST(parser.error_status() == 505);
}

BOOST_AUTO_TEST_CASE(EqualDuplicateContentLength_IsAccepted)
{
	mcp::http_request_parser parser;
	BOOST_REQUIRE(parser.feed("POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 2\r\n\r\nab") == state::complete);
	BOOST_TEST(parser.take().body == "ab");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(McpHttpResponseTests)

BOOST_AUTO_TEST_CASE(Serialize_WritesStatusHeadersLengthAndBody)
{
	mcp::http_response response;
	response.status = 401;
	response.set_header("WWW-Authenticate", "Bearer");
	response.body = "nope";
	response.close = true;

	BOOST_TEST(mcp::serialize_response(response)
		== "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer\r\nContent-Length: 4\r\nConnection: close\r\n\r\nnope");
}

BOOST_AUTO_TEST_CASE(SetHeader_ReplacesACaseInsensitiveDuplicate)
{
	mcp::http_response response;
	response.set_header("Content-Type", "text/plain");
	response.set_header("content-type", "application/json");
	BOOST_REQUIRE_EQUAL(response.headers.size(), 1u);
	BOOST_TEST(response.headers.front().value == "application/json");
}

BOOST_AUTO_TEST_CASE(StreamHead_HasNoContentLength)
{
	mcp::http_response response;
	response.set_header("Content-Type", "text/event-stream");
	BOOST_TEST(mcp::serialize_stream_head(response)
		== "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: keep-alive\r\n\r\n");
}

BOOST_AUTO_TEST_CASE(ReasonPhrase_NamesTheStatusesTheServerSends)
{
	BOOST_TEST(mcp::reason_phrase(500) == "Internal Server Error");
	BOOST_TEST(mcp::reason_phrase(403) == "Forbidden");
	BOOST_TEST(mcp::reason_phrase(401) == "Unauthorized");
	BOOST_TEST(mcp::reason_phrase(413) == "Payload Too Large");
	mcp::http_response response;
	response.status = 500;
	BOOST_TEST(mcp::serialize_response(response).rfind("HTTP/1.1 500 Internal Server Error\r\n", 0) == 0u);
}

BOOST_AUTO_TEST_CASE(SseMessage_IsOneEvent)
{
	BOOST_TEST(mcp::sse_message("{\"a\":1}") == "event: message\ndata: {\"a\":1}\n\n");
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(McpEventChannelTests)

BOOST_AUTO_TEST_CASE(PushAndTake_KeepTheOrder)
{
	mcp::event_channel channel;
	BOOST_TEST(channel.push("a"));
	BOOST_TEST(channel.push("b"));
	BOOST_TEST((channel.take() == std::vector<std::string>{ "a", "b" }));
	BOOST_TEST(channel.take().empty());
}

BOOST_AUTO_TEST_CASE(Wake_RunsOnPushAndOnCloseAndAtOnceForWaitingFrames)
{
	mcp::event_channel channel;
	int wakes = 0;
	channel.push("early");
	channel.set_wake([&wakes] { ++wakes; });
	BOOST_TEST(wakes == 1);
	channel.push("late");
	BOOST_TEST(wakes == 2);
	channel.close();
	BOOST_TEST(wakes == 3);
	BOOST_TEST(channel.closed());
}

BOOST_AUTO_TEST_CASE(Closed_RefusesFrames)
{
	mcp::event_channel channel;
	channel.close();
	BOOST_TEST(!channel.push("x"));
	BOOST_TEST(channel.take().empty());
}

BOOST_AUTO_TEST_CASE(ClientThatDoesNotRead_ClosesTheChannelAtTheLimit)
{
	mcp::event_channel channel(2);
	BOOST_TEST(channel.push("1"));
	BOOST_TEST(channel.push("2"));
	BOOST_TEST(!channel.push("3"));
	BOOST_TEST(channel.closed());
	BOOST_TEST((channel.take() == std::vector<std::string>{ "1", "2" }));
}

BOOST_AUTO_TEST_CASE(PushFromManyThreads_LosesNothing)
{
	mcp::event_channel channel(100000);
	std::atomic<int> wakes{ 0 };
	channel.set_wake([&wakes] { ++wakes; });
	constexpr int per_thread = 500;
	std::vector<std::thread> threads;
	for (int t = 0; t < 4; ++t) {
		threads.emplace_back([&channel] {
			for (int i = 0; i < per_thread; ++i) {
				channel.push("x");
			}
		});
	}
	for (std::thread& thread : threads) {
		thread.join();
	}
	BOOST_TEST(channel.take().size() == static_cast<std::size_t>(4 * per_thread));
	BOOST_TEST(wakes.load() == 4 * per_thread);
}

BOOST_AUTO_TEST_SUITE_END()
