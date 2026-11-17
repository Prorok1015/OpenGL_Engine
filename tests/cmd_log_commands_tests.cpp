#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_log_commands.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/logger/engine_log.h>
#include <boost/json/parse.hpp>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <vector>

// log.read on a command_registry of its own: the same registration a game without the editor makes.

namespace {

namespace json = boost::json;

struct log_command_fixture
{
	log_command_fixture()
		: previous_capacity(engine::log_journal_capacity())
		, previous_level(engine::log_journal_min_level())
		, registration(cmd::register_log_commands(commands))
	{
		engine::set_log_journal_capacity(1000);
		engine::set_log_journal_min_level(engine::log_level::trace); // the tests log debug on purpose
	}
	~log_command_fixture()
	{
		engine::set_log_journal_capacity(previous_capacity);
		engine::set_log_journal_min_level(previous_level);
	}

	// `fields` is the inside of the argument object, e.g. R"("category":"x")".
	cmd::result read(const std::string& fields)
	{
		return commands.invoke("log.read", json::parse("{" + fields + "}"), cmd::origin::test());
	}

	static std::vector<std::string> texts(const cmd::result& result)
	{
		std::vector<std::string> out;
		for (const auto& entry : result.value.as_object().at("entries").as_array()) {
			out.emplace_back(entry.as_object().at("text").as_string());
		}
		return out;
	}

	std::size_t previous_capacity;
	std::optional<engine::log_level> previous_level;
	cmd::command_registry commands;
	cmd::registration registration;
};

}

BOOST_FIXTURE_TEST_SUITE(CmdLogCommandsTests, log_command_fixture)

BOOST_AUTO_TEST_CASE(Registered_WithSchemasAndListedAsATool)
{
	BOOST_REQUIRE(registration.active());
	const cmd::command_desc* desc = commands.find("log.read");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_TEST((desc->category == cmd::agent_category::read));
	BOOST_CHECK(desc->expose_as_tool);
	BOOST_CHECK(!desc->mutates);
	BOOST_CHECK(desc->args_schema.as_object().contains("properties"));

	cmd::command_registry other;
	auto twice = cmd::register_log_commands(other);
	BOOST_CHECK(twice.active());
	auto duplicate = cmd::register_log_commands(other);
	BOOST_CHECK(!duplicate.active()); // the id is taken
}

BOOST_AUTO_TEST_CASE(Read_AnswersEntriesNextSeqAndDropped)
{
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_WARN("covcmd/shape", "value={}", 7);

	const cmd::result result = read(std::format(R"("since_seq":{},"category":"covcmd/shape")", first));
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const json::object& body = result.value.as_object();
	BOOST_CHECK_EQUAL(body.at("next_seq").as_int64(), static_cast<std::int64_t>(first + 1));
	BOOST_CHECK(!body.at("dropped").as_bool());

	const json::array& entries = body.at("entries").as_array();
	BOOST_REQUIRE_EQUAL(entries.size(), 1u);
	const json::object& entry = entries[0].as_object();
	BOOST_CHECK_EQUAL(entry.at("seq").as_int64(), static_cast<std::int64_t>(first));
	BOOST_CHECK_EQUAL(entry.at("level").as_string().c_str(), "warn");
	BOOST_CHECK_EQUAL(entry.at("category").as_string().c_str(), "covcmd/shape");
	BOOST_CHECK_EQUAL(entry.at("text").as_string().c_str(), "value=7");
	BOOST_CHECK(entry.at("time_ms").as_int64() > 0);
}

BOOST_AUTO_TEST_CASE(Read_FiltersByMinLevelAndCategoryBranch)
{
	const std::uint64_t first = engine::log_journal_next_seq();
	egLOG_DEBUG("covcmd/flt", "debug");
	egLOG_INFO("covcmd/flt", "info");
	egLOG_WARN("covcmd/flt/sub", "warn");
	egLOG_ERROR("covcmd/fltx", "other branch");
	const std::string since = std::format(R"("since_seq":{})", first);

	BOOST_TEST((texts(read(since + R"(,"category":"covcmd/flt")")) == std::vector<std::string>{ "debug", "info", "warn" }));
	BOOST_TEST((texts(read(since + R"(,"category":"covcmd/flt","min_level":"info")")) == std::vector<std::string>{ "info", "warn" }));
	BOOST_TEST((texts(read(since + R"(,"category":"covcmd/flt","min_level":"warn")")) == std::vector<std::string>{ "warn" }));
	BOOST_TEST((texts(read(since + R"(,"min_level":"error")")) == std::vector<std::string>{ "other branch" }));
}

BOOST_AUTO_TEST_CASE(Read_LimitSplitsIntoPagesThatContinueAtNextSeq)
{
	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 5; ++i) egLOG_INFO("covcmd/page", "p{}", i);

	std::vector<std::string> seen;
	std::int64_t since = static_cast<std::int64_t>(first);
	for (int loops = 0; loops < 10; ++loops) {
		const cmd::result page = read(std::format(R"("since_seq":{},"category":"covcmd/page","limit":2)", since));
		BOOST_REQUIRE(page.ok);
		for (auto& text : texts(page)) seen.push_back(std::move(text));
		if (page.value.as_object().at("entries").as_array().size() < 2u) break;
		since = page.value.as_object().at("next_seq").as_int64();
	}
	BOOST_TEST((seen == std::vector<std::string>{ "p0", "p1", "p2", "p3", "p4" }));
}

BOOST_AUTO_TEST_CASE(Read_WithoutLimit_ReturnsAtMostAHundred)
{
	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 120; ++i) egLOG_INFO("covcmd/many", "n{}", i);

	const cmd::result result = read(std::format(R"("since_seq":{},"category":"covcmd/many")", first));
	BOOST_REQUIRE(result.ok);
	BOOST_CHECK_EQUAL(result.value.as_object().at("entries").as_array().size(), 100u);
	BOOST_CHECK_EQUAL(result.value.as_object().at("next_seq").as_int64(), static_cast<std::int64_t>(first + 100));

	const cmd::result biggest = read(std::format(R"("since_seq":{},"category":"covcmd/many","limit":500)", first));
	BOOST_CHECK_EQUAL(biggest.value.as_object().at("entries").as_array().size(), 120u);
}

BOOST_AUTO_TEST_CASE(Read_SaysDroppedWhenSinceSeqLeftTheRing)
{
	engine::set_log_journal_capacity(3);
	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 5; ++i) egLOG_INFO("covcmd/ring", "r{}", i);

	const cmd::result stale = read(std::format(R"("since_seq":{},"category":"covcmd/ring")", first));
	BOOST_REQUIRE(stale.ok);
	BOOST_CHECK(stale.value.as_object().at("dropped").as_bool());
	BOOST_TEST((texts(stale) == std::vector<std::string>{ "r2", "r3", "r4" }));

	const cmd::result fresh = read(std::format(R"("since_seq":{},"category":"covcmd/ring")", first + 2));
	BOOST_CHECK(!fresh.value.as_object().at("dropped").as_bool());
	BOOST_TEST((texts(fresh) == std::vector<std::string>{ "r2", "r3", "r4" }));
}

// A JSON writer may send 5.0 for an integer; the schema accepts it, so the command must read it instead of throwing.
BOOST_AUTO_TEST_CASE(Read_AcceptsIntegralDoublesForItsIntegerArguments)
{
	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 4; ++i) egLOG_INFO("covcmd/dbl", "d{}", i);

	const cmd::result result = read(std::format(R"("since_seq":{}.0,"category":"covcmd/dbl","limit":2.0)", first));
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST((texts(result) == std::vector<std::string>{ "d0", "d1" }));
	BOOST_CHECK_EQUAL(result.value.as_object().at("next_seq").as_int64(), static_cast<std::int64_t>(first + 2));

	BOOST_CHECK_EQUAL(read(R"("limit":600.0)").code, "invalid_args");
	BOOST_CHECK_EQUAL(read(R"("limit":2.5)").code, "invalid_args");
	BOOST_CHECK_NO_THROW(read(R"("since_seq":1e30)"));
}

BOOST_AUTO_TEST_CASE(Read_IsAReadOnlyCommand)
{
	const cmd::command_desc* desc = commands.find("log.read");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_CHECK(!desc->mutates);
	BOOST_CHECK(desc->risk == cmd::danger::none);
}

BOOST_AUTO_TEST_CASE(Read_RejectsArgumentsThatBreakTheSchema)
{
	BOOST_CHECK_EQUAL(read(R"("limit":501)").code, "invalid_args");
	BOOST_CHECK_EQUAL(read(R"("limit":0)").code, "invalid_args");
	BOOST_CHECK_EQUAL(read(R"("since_seq":-1)").code, "invalid_args");
	BOOST_CHECK_EQUAL(read(R"("min_level":"loud")").code, "invalid_args");
	BOOST_CHECK_EQUAL(read(R"("category":5)").code, "invalid_args");
	BOOST_CHECK_EQUAL(read(R"("nope":true)").code, "invalid_args");
	BOOST_CHECK(read("").ok);
}

// #935: whoever owns the callers' rights says what a caller gets of the journal.
BOOST_AUTO_TEST_CASE(AFilter_LeavesEntriesOutWithoutCountingThem_AndReplacesTheText)
{
	cmd::log_read_filter filter;
	filter.visible = [](std::string_view category, const cmd::origin&) { return category != "log_filter_test/hidden"; };
	filter.redact = [](std::string_view text, const cmd::origin& from) {
		return (from.kind == cmd::origin_kind::agent ? "agent[" : "[") + std::string(text) + "]";
	};
	cmd::command_registry filtered;
	const cmd::registration filtered_registration = cmd::register_log_commands(filtered, &filter);
	const auto read_filtered = [&filtered](const std::string& fields, cmd::origin from = cmd::origin::test()) {
		return filtered.invoke("log.read", json::parse("{" + fields + "}"), std::move(from));
	};

	egLOG_INFO("log_filter_test/shown", "a");
	for (int i = 0; i < 3; ++i) egLOG_INFO("log_filter_test/hidden", "secret {}", i);
	egLOG_INFO("log_filter_test/shown", "b");
	egLOG_INFO("log_filter_test/hidden", "secret again");
	egLOG_INFO("log_filter_test/shown", "c");

	// The hidden entries between them do not use up the limit.
	const auto first = read_filtered(R"("category":"log_filter_test","limit":2)");
	BOOST_REQUIRE_MESSAGE(first.ok, first.error);
	BOOST_TEST((texts(first) == std::vector<std::string>{ "[a]", "[b]" }));
	const auto next = first.value.as_object().at("next_seq").as_int64();
	const auto rest = read_filtered(R"("category":"log_filter_test","since_seq":)" + std::to_string(next));
	BOOST_REQUIRE_MESSAGE(rest.ok, rest.error);
	BOOST_TEST((texts(rest) == std::vector<std::string>{ "[c]" }));

	// The filter is told who asks.
	const auto as_agent = read_filtered(R"("category":"log_filter_test","limit":1)", cmd::origin::agent("s1"));
	BOOST_TEST((texts(as_agent) == std::vector<std::string>{ "agent[a]" }));

	// A registry without a filter gives the journal as it is.
	const auto whole = read(R"("category":"log_filter_test")");
	BOOST_TEST(texts(whole).size() == 7u);
}

// A small limit behind many hidden entries still gets the next shown one: the journal is asked for far more than it.
BOOST_AUTO_TEST_CASE(AFilteredRead_IsNotEmptyBecauseOfTheEntriesLeftOut)
{
	cmd::log_read_filter filter;
	filter.visible = [](std::string_view category, const cmd::origin&) { return category != "log_page_test/hidden"; };
	cmd::command_registry filtered;
	const cmd::registration filtered_registration = cmd::register_log_commands(filtered, &filter);
	const auto read_from = [&filtered](std::uint64_t since) {
		return filtered.invoke("log.read", json::parse(std::format(R"({{"category":"log_page_test","limit":1,"since_seq":{}}})", since)),
			cmd::origin::agent("s1"));
	};

	const std::uint64_t first = engine::log_journal_next_seq();
	for (int i = 0; i < 200; ++i) egLOG_INFO("log_page_test/hidden", "secret {}", i);
	egLOG_INFO("log_page_test/shown", "found");
	egLOG_INFO("log_page_test/shown", "later");

	const auto page = read_from(first);
	BOOST_REQUIRE_MESSAGE(page.ok, page.error);
	BOOST_TEST((texts(page) == std::vector<std::string>{ "found" }));
	const auto rest = read_from(static_cast<std::uint64_t>(page.value.as_object().at("next_seq").as_int64()));
	BOOST_REQUIRE_MESSAGE(rest.ok, rest.error);
	BOOST_TEST((texts(rest) == std::vector<std::string>{ "later" }));
}

BOOST_AUTO_TEST_SUITE_END()
