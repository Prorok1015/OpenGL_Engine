#include <boost/test/unit_test.hpp>
#include "mcp_test_support.hpp"
#include <snk/mcp/mcp_stdio.h>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// The stdio transport of snk_mcp_server (#452) without a real stdin/stdout: lines go in through handle_line or a scripted
// byte source, the output is captured line by line. Same fake host as the endpoint tests.

namespace {

namespace json = boost::json;
using mcp_test::fake_host;
using mcp_test::json_of;

std::string text_of(const json::value& value)
{
	const json::string& text = value.as_string();
	return std::string(text.data(), text.size());
}

// Every chunk the transport writes; each must be one JSON-RPC line.
class captured_output
{
public:
	std::function<bool(std::string_view)> writer()
	{
		return [this](std::string_view chunk) {
			std::lock_guard lock(m_mutex);
			m_chunks.emplace_back(chunk);
			return true;
		};
	}

	std::vector<std::string> chunks() const
	{
		std::lock_guard lock(m_mutex);
		return m_chunks;
	}

	std::vector<json::value> messages() const
	{
		std::vector<json::value> out;
		for (const std::string& chunk : chunks()) {
			out.push_back(json::parse(chunk));
		}
		return out;
	}

	json::value last() const
	{
		const std::vector<json::value> all = messages();
		BOOST_REQUIRE(!all.empty());
		return all.back();
	}

	std::size_t size() const
	{
		std::lock_guard lock(m_mutex);
		return m_chunks.size();
	}

	// Exactly one '\n', at the end, and a JSON-RPC 2.0 object before it.
	bool all_well_formed() const
	{
		for (const std::string& chunk : chunks()) {
			if (chunk.empty() || chunk.back() != '\n' || std::count(chunk.begin(), chunk.end(), '\n') != 1) {
				return false;
			}
			const json::value message = json::parse(chunk);
			if (!message.is_object() || !message.as_object().contains("jsonrpc") || text_of(message.at("jsonrpc")) != "2.0") {
				return false;
			}
		}
		return true;
	}

private:
	mutable std::mutex m_mutex;
	std::vector<std::string> m_chunks;
};

// Hands out the chunks it was given, then the end of the input.
class scripted_source final : public mcp::byte_source
{
public:
	explicit scripted_source(std::vector<std::string> chunks) : m_chunks(std::move(chunks)) {}

	std::size_t read(char* buffer, std::size_t size) override
	{
		if (m_next >= m_chunks.size()) {
			return 0;
		}
		std::string& chunk = m_chunks[m_next];
		const std::size_t n = std::min(size, chunk.size());
		std::memcpy(buffer, chunk.data(), n);
		chunk.erase(0, n);
		if (chunk.empty()) {
			++m_next;
		}
		return n;
	}

	void interrupt() override {}

private:
	std::vector<std::string> m_chunks;
	std::size_t m_next = 0;
};

// Never sends anything: blocks until interrupted, like a client that is still there.
class silent_source final : public mcp::byte_source
{
public:
	std::size_t read(char*, std::size_t) override
	{
		std::unique_lock lock(m_mutex);
		m_cv.wait(lock, [this] { return m_interrupted; });
		return 0;
	}

	void interrupt() override
	{
		{
			std::lock_guard lock(m_mutex);
			m_interrupted = true;
		}
		m_cv.notify_all();
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_cv;
	bool m_interrupted = false;
};

struct eof_flag
{
	std::mutex mutex;
	std::condition_variable cv;
	int count = 0;

	std::function<void()> callback()
	{
		return [this] {
			{
				std::lock_guard lock(mutex);
				++count;
			}
			cv.notify_all();
		};
	}

	int calls()
	{
		std::lock_guard lock(mutex);
		return count;
	}

	bool wait()
	{
		std::unique_lock lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(20), [this] { return count > 0; });
	}
};

void feed(mcp::stdio_transport& transport, std::string_view line)
{
	transport.handle_line(line);
	BOOST_REQUIRE(transport.flush());
}

// A writer that blocks every write until released, like a client that stopped reading its stdout.
struct gated_writer
{
	std::mutex mutex;
	std::condition_variable cv;
	bool open = false;
	int writes = 0;

	std::function<bool(std::string_view)> writer()
	{
		return [this](std::string_view) {
			std::unique_lock lock(mutex);
			++writes;
			cv.wait(lock, [this] { return open; });
			return true;
		};
	}

	void release()
	{
		{
			std::lock_guard lock(mutex);
			open = true;
		}
		cv.notify_all();
	}
};

// Blocks until released and ignores interrupt(), like a console read that cannot be cancelled.
struct stuck_input
{
	std::mutex mutex;
	std::condition_variable cv;
	bool released = false;
	bool returned = false;

	void release()
	{
		{
			std::lock_guard lock(mutex);
			released = true;
		}
		cv.notify_all();
	}

	bool wait_returned()
	{
		std::unique_lock lock(mutex);
		return cv.wait_for(lock, std::chrono::seconds(20), [this] { return returned; });
	}
};

class stuck_source final : public mcp::byte_source
{
public:
	explicit stuck_source(std::shared_ptr<stuck_input> input) : m_input(std::move(input)) {}

	std::size_t read(char*, std::size_t) override
	{
		std::unique_lock lock(m_input->mutex);
		m_input->cv.wait(lock, [this] { return m_input->released; });
		m_input->returned = true;
		m_input->cv.notify_all();
		return 0;
	}

	void interrupt() override {}

private:
	std::shared_ptr<stuck_input> m_input;
};

const char* const initialize_line = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}})";

struct stdio_fixture
{
	stdio_fixture()
	{
		host.set_tools({ mcp_test::make_tool("probe") });
	}

	mcp::stdio_config config()
	{
		mcp::stdio_config c;
		c.info.name = "test-server";
		c.write = output.writer();
		c.on_eof = eof.callback();
		return c;
	}

	fake_host host;
	captured_output output;
	eof_flag eof;
};

}

BOOST_AUTO_TEST_SUITE(McpStdioTests)

BOOST_FIXTURE_TEST_CASE(Initialize_ThenToolsListAndCall_AnswerLineByLine, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	BOOST_TEST(transport.session_id().empty());

	feed(transport, initialize_line);
	const json::value init = output.last();
	BOOST_TEST(init.at("id").as_int64() == 1);
	BOOST_TEST(text_of(init.at("result").at("protocolVersion")) == "2025-06-18");
	BOOST_TEST(text_of(init.at("result").at("serverInfo").at("name")) == "test-server");
	const std::string session = transport.session_id();
	BOOST_TEST(session.size() == 32u);

	feed(transport, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
	BOOST_TEST(output.size() == 1u);

	feed(transport, R"({"jsonrpc":"2.0","id":"two","method":"tools/list"})");
	const json::value list = output.last();
	BOOST_TEST(text_of(list.at("id")) == "two");
	BOOST_TEST(text_of(list.at("result").at("tools").as_array().at(0).at("name")) == "probe");

	mcp::call_result answer;
	answer.structured = json_of(R"({"n":3})");
	host.set_next_result(answer);
	// A '\r' before the '\n' (a Windows client) is not part of the message.
	feed(transport, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"probe\",\"arguments\":{\"n\":3}}}\r");
	const json::value called = output.last();
	BOOST_TEST(called.at("id").as_int64() == 3);
	BOOST_TEST(called.at("result").at("structuredContent").at("n").as_int64() == 3);
	BOOST_TEST(!called.at("result").at("isError").as_bool());
	BOOST_REQUIRE(host.calls().size() == 1u);
	BOOST_TEST(host.calls()[0].session_id == session);
	BOOST_TEST(host.calls()[0].name == "probe");

	feed(transport, R"({"jsonrpc":"2.0","id":4,"method":"ping"})");
	BOOST_TEST(output.last().at("result").as_object().empty());
	feed(transport, R"({"jsonrpc":"2.0","id":5,"method":"no/such"})");
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32601);

	BOOST_TEST(output.all_well_formed());
}

BOOST_FIXTURE_TEST_CASE(ToolsChanged_IsNotifiedToAnOpenSessionOnly, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	transport.notify_tools_changed();
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(output.size() == 0u);

	feed(transport, initialize_line);
	transport.notify_tools_changed();
	BOOST_REQUIRE(transport.flush());
	BOOST_REQUIRE(output.size() == 2u);
	const json::value note = output.last();
	BOOST_TEST(text_of(note.at("method")) == "notifications/tools/list_changed");
	BOOST_TEST(!note.as_object().contains("id"));
	BOOST_TEST(output.all_well_formed());
}

BOOST_FIXTURE_TEST_CASE(Progress_IsWrittenOnlyForACallWithAToken, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	feed(transport, initialize_line);
	host.set_defer_calls(true);
	feed(transport, R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"probe","_meta":{"progressToken":"p-1"}}})");
	feed(transport, R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"probe"}})");
	BOOST_TEST(output.size() == 1u);

	const auto calls = host.calls();
	BOOST_REQUIRE(calls.size() == 2u);
	calls[0].progress(1.0, 4.0, "a quarter");
	calls[1].progress(1.0, 4.0, "nobody listens for a token");
	BOOST_REQUIRE(transport.flush());
	BOOST_REQUIRE(output.size() == 2u);
	const json::value note = output.last();
	BOOST_TEST(text_of(note.at("method")) == "notifications/progress");
	BOOST_TEST(text_of(note.at("params").at("progressToken")) == "p-1");
	BOOST_TEST(note.at("params").at("progress").to_number<double>() == 1.0);
	BOOST_TEST(note.at("params").at("total").to_number<double>() == 4.0);
	BOOST_TEST(text_of(note.at("params").at("message")) == "a quarter");

	BOOST_TEST(host.answer_deferred(mcp::call_result{}));
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(output.last().at("id").as_int64() == 9);
	BOOST_TEST(output.all_well_formed());
}

BOOST_FIXTURE_TEST_CASE(BadInput_IsAnsweredWithTheErrorsHttpGives, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());

	feed(transport, "this is not json");
	json::value error = output.last();
	BOOST_TEST(error.at("id").is_null());
	BOOST_TEST(error.at("error").at("code").as_int64() == -32700);

	feed(transport, R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])");
	error = output.last();
	BOOST_TEST(error.at("id").is_null());
	BOOST_TEST(error.at("error").at("code").as_int64() == -32600);

	// Before initialize there is no session: a request is refused, a notification is dropped.
	feed(transport, R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})");
	error = output.last();
	BOOST_TEST(error.at("id").as_int64() == 7);
	BOOST_TEST(error.at("error").at("code").as_int64() == -32600);
	const std::size_t before = output.size();
	feed(transport, R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
	feed(transport, "");
	BOOST_TEST(output.size() == before);
	BOOST_TEST(host.calls().empty());

	feed(transport, initialize_line);
	feed(transport, R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":[1]})");
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32602);
	feed(transport, R"({"jsonrpc":"2.0","id":null,"method":"ping"})");
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32600);
	BOOST_TEST(output.all_well_formed());
}

BOOST_FIXTURE_TEST_CASE(AHostThatThrows_IsAnInternalError, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	feed(transport, initialize_line);
	host.set_throw_on_list(true);
	feed(transport, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32603);
	BOOST_TEST(output.last().at("id").as_int64() == 2);
}

BOOST_FIXTURE_TEST_CASE(Subscriptions_GetResourceUpdates, stdio_fixture)
{
	mcp::resource_info log;
	log.uri = "snk://log";
	log.name = "log";
	host.set_resources({ log });
	mcp::stdio_transport transport(host, config());
	feed(transport, initialize_line);
	transport.notify_resource_updated("snk://log");
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(output.size() == 1u);

	feed(transport, R"({"jsonrpc":"2.0","id":2,"method":"resources/subscribe","params":{"uri":"snk://nope"}})");
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32602);
	feed(transport, R"({"jsonrpc":"2.0","id":3,"method":"resources/subscribe","params":{"uri":"snk://log"}})");
	BOOST_TEST(output.last().at("result").as_object().empty());
	transport.notify_resource_updated("snk://log");
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(text_of(output.last().at("method")) == "notifications/resources/updated");
	BOOST_TEST(text_of(output.last().at("params").at("uri")) == "snk://log");

	feed(transport, R"({"jsonrpc":"2.0","id":4,"method":"resources/unsubscribe","params":{"uri":"snk://log"}})");
	const std::size_t before = output.size();
	transport.notify_resource_updated("snk://log");
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(output.size() == before);
}

BOOST_FIXTURE_TEST_CASE(EndOfInput_ClosesTheSessionThenCallsOnEof, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	const std::string init = initialize_line;
	// Lines split across reads, a CRLF, and a last line without its '\n'.
	transport.start(std::make_unique<scripted_source>(std::vector<std::string>{
		init.substr(0, 20), init.substr(20) + "\r\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n",
		R"({"jsonrpc":"2.0","id":2,"method":"ping"})" }));
	BOOST_REQUIRE(eof.wait());
	BOOST_REQUIRE(transport.flush());

	const std::vector<json::value> messages = output.messages();
	BOOST_REQUIRE(messages.size() == 2u);
	BOOST_TEST(messages[0].at("id").as_int64() == 1);
	BOOST_TEST(messages[1].at("id").as_int64() == 2);
	BOOST_TEST(output.all_well_formed());
	BOOST_TEST(host.closed_sessions().size() == 1u);
	BOOST_TEST(host.closed_sessions()[0].size() == 32u);
	BOOST_TEST(transport.session_id().empty());

	transport.stop();
	BOOST_TEST(eof.calls() == 1);
	BOOST_TEST(host.closed_sessions().size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(ALineTooLong_IsRefusedAndEndsTheInput, stdio_fixture)
{
	mcp::stdio_config c = config();
	c.max_line_bytes = 16;
	BOOST_TEST(mcp::stdio_config{}.max_line_bytes == 4u * 1024u * 1024u);
	mcp::stdio_transport transport(host, std::move(c));
	transport.start(std::make_unique<scripted_source>(std::vector<std::string>{ std::string(40, 'x') }));
	BOOST_REQUIRE(eof.wait());
	BOOST_REQUIRE(transport.flush());
	const json::value error = output.last();
	BOOST_TEST(error.at("id").is_null());
	BOOST_TEST(error.at("error").at("code").as_int64() == -32600);
	BOOST_TEST(output.size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(Stop_EndsTheReaderWithoutOnEof_AndClosesTheSession, stdio_fixture)
{
	{
		mcp::stdio_transport transport(host, config());
		feed(transport, initialize_line);
		const std::string session = transport.session_id();
		transport.start(std::make_unique<silent_source>());
		transport.stop();
		BOOST_REQUIRE(host.closed_sessions().size() == 1u);
		BOOST_TEST(host.closed_sessions()[0] == session);
	}
	BOOST_TEST(eof.calls() == 0);
	BOOST_TEST(host.closed_sessions().size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(ASecondInitialize_ReplacesTheSession, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	feed(transport, initialize_line);
	const std::string first = transport.session_id();
	feed(transport, initialize_line);
	BOOST_TEST(transport.session_id() != first);
	BOOST_REQUIRE(host.closed_sessions().size() == 1u);
	BOOST_TEST(host.closed_sessions()[0] == first);
}

BOOST_FIXTURE_TEST_CASE(NulBytesAndInvalidUtf8_AreParseErrors, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	std::string with_nul = R"({"jsonrpc":"2.0",)";
	with_nul.push_back('\0');
	with_nul += R"("id":1,"method":"ping"})";
	feed(transport, with_nul);
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32700);
	feed(transport, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\xff\xfe\"}");
	BOOST_TEST(output.last().at("error").at("code").as_int64() == -32700);
	BOOST_TEST(output.last().at("id").is_null());
	BOOST_TEST(output.size() == 2u);
	BOOST_TEST(output.all_well_formed());
}

BOOST_FIXTURE_TEST_CASE(AClientThatDoesNotRead_NeverBlocksTheSender, stdio_fixture)
{
	gated_writer gate;
	mcp::stdio_config c = config();
	c.write = gate.writer();
	mcp::stdio_transport transport(host, std::move(c));
	transport.handle_line(initialize_line);
	// The writer now hangs in the first line; whatever the host answers is only queued.
	std::atomic<bool> sent{ false };
	std::thread sender([&] {
		for (int i = 0; i < 100; ++i) {
			transport.handle_line(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
			transport.notify_tools_changed();
		}
		sent = true;
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!sent && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	const bool returned = sent;
	gate.release();
	sender.join();
	BOOST_TEST(returned);
	BOOST_TEST(transport.flush());
	BOOST_TEST(eof.calls() == 0);
}

BOOST_FIXTURE_TEST_CASE(AnOutputOverTheLimit_EndsTheSessionOnce, stdio_fixture)
{
	gated_writer gate;
	mcp::stdio_config c = config();
	c.write = gate.writer();
	c.max_output_bytes = 1024;
	mcp::stdio_transport transport(host, std::move(c));
	transport.handle_line(initialize_line);
	const std::string session = transport.session_id();
	for (int i = 0; i < 100; ++i) {
		transport.notify_tools_changed();
	}
	// Ended while the writer is still stuck.
	BOOST_REQUIRE(eof.wait());
	BOOST_TEST(transport.session_id().empty());
	BOOST_REQUIRE(host.closed_sessions().size() == 1u);
	BOOST_TEST(host.closed_sessions()[0] == session);
	gate.release();
	BOOST_TEST(transport.flush());
	transport.handle_line(R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
	transport.stop();
	BOOST_TEST(eof.calls() == 1);
	// The one line taken before the overflow is all that was written.
	std::lock_guard lock(gate.mutex);
	BOOST_TEST(gate.writes == 1);
}

BOOST_FIXTURE_TEST_CASE(AWriteError_EndsTheSessionOnce, stdio_fixture)
{
	mcp::stdio_config c = config();
	c.write = [](std::string_view) { return false; };
	mcp::stdio_transport transport(host, std::move(c));
	transport.handle_line(initialize_line);
	BOOST_REQUIRE(eof.wait());
	transport.handle_line(initialize_line);
	transport.notify_tools_changed();
	transport.stop();
	BOOST_TEST(eof.calls() == 1);
}

BOOST_FIXTURE_TEST_CASE(TwoSendingThreads_WriteWholeLines, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	feed(transport, initialize_line);
	const auto spam = [&transport] {
		for (int i = 0; i < 300; ++i) {
			transport.notify_tools_changed();
		}
	};
	std::thread a(spam);
	std::thread b(spam);
	a.join();
	b.join();
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(output.size() == 601u);
	BOOST_TEST(output.all_well_formed());
}

BOOST_FIXTURE_TEST_CASE(AnAnswerToAReplacedSession_IsDropped, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	feed(transport, initialize_line);
	host.set_defer_calls(true);
	feed(transport, R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"probe","_meta":{"progressToken":"p"}}})");
	BOOST_REQUIRE(host.calls().size() == 1u);
	feed(transport, initialize_line);
	const std::size_t before = output.size();
	host.calls()[0].progress(1.0, 2.0, "late");
	BOOST_TEST(host.answer_deferred(mcp::call_result{}));
	BOOST_REQUIRE(transport.flush());
	BOOST_TEST(output.size() == before);
}

BOOST_FIXTURE_TEST_CASE(TheInitializeResult_PrecedesEveryNotificationOfItsSession, stdio_fixture)
{
	mcp::stdio_transport transport(host, config());
	std::atomic<bool> done{ false };
	std::thread notifier([&] {
		while (!done) {
			transport.notify_tools_changed();
		}
	});
	transport.handle_line(initialize_line);
	done = true;
	notifier.join();
	BOOST_REQUIRE(transport.flush());
	const std::vector<json::value> messages = output.messages();
	BOOST_REQUIRE(!messages.empty());
	BOOST_TEST(messages[0].at("id").as_int64() == 1);
	BOOST_TEST(messages[0].as_object().contains("result"));
}

BOOST_FIXTURE_TEST_CASE(AReadThatIgnoresInterrupt_DoesNotHangStop, stdio_fixture)
{
	auto input = std::make_shared<stuck_input>();
	{
		mcp::stdio_config c = config();
		c.stop_timeout = std::chrono::milliseconds(200);
		mcp::stdio_transport transport(host, std::move(c));
		transport.start(std::make_unique<stuck_source>(input));
		feed(transport, initialize_line);
		const auto before = std::chrono::steady_clock::now();
		transport.stop();
		BOOST_TEST((std::chrono::steady_clock::now() - before < std::chrono::seconds(10)));
		BOOST_TEST(host.closed_sessions().size() == 1u);
	}
	// The reader left behind ends when its read returns, and calls nothing.
	input->release();
	BOOST_REQUIRE(input->wait_returned());
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	BOOST_TEST(eof.calls() == 0);
	BOOST_TEST(host.closed_sessions().size() == 1u);
}

BOOST_FIXTURE_TEST_CASE(AfterTheOutputDied_InputReachesNoHost, stdio_fixture)
{
	mcp::stdio_config c = config();
	c.write = [](std::string_view) { return false; };
	mcp::stdio_transport transport(host, std::move(c));
	transport.handle_line(initialize_line);
	BOOST_REQUIRE(eof.wait());
	transport.handle_line(initialize_line);
	transport.handle_line(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"probe"}})");
	BOOST_TEST(transport.session_id().empty());
	BOOST_TEST(host.calls().empty());
	transport.stop();
	BOOST_TEST(eof.calls() == 1);
}

BOOST_AUTO_TEST_SUITE_END()
