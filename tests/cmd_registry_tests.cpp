#include <boost/test/unit_test.hpp>
#include <snk/command/cmd_registry.h>
#include <snk/command/cmd_policy.h>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/monotonic_resource.hpp>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <utility>
#include <vector>

// cmd::command_registry on its own: no editor, no ImGui, no window. The editor's action_registry sits on top of
// it (edt_action_registry_tests.cpp); main_thread_queue has its own file.

namespace {

boost::json::value json_of(std::string_view text)
{
	return boost::json::parse(text);
}

cmd::command_desc make_command(std::string id, std::function<cmd::result(const cmd::call&)> body = nullptr)
{
	cmd::command_desc desc;
	desc.id = std::move(id);
	desc.invoke = body ? std::move(body) : [](const cmd::call&) { return cmd::result::success(); };
	return desc;
}

cmd::command_desc echo_command(int& runs)
{
	cmd::command_desc desc = make_command("test.echo", [&runs](const cmd::call& request) {
		++runs;
		return cmd::result::success(boost::json::object{ { "echo", request.args.as_object().at("text") } });
	});
	desc.summary = "Echoes the text";
	desc.args_schema = json_of(R"({"type":"object","properties":{"text":{"type":"string","minLength":1},"times":{"type":"integer","minimum":1}},
		"required":["text"],"additionalProperties":false})");
	desc.result_schema = json_of(R"({"type":"object","properties":{"echo":{"type":"string"}},"required":["echo"],"additionalProperties":false})");
	return desc;
}

}

BOOST_AUTO_TEST_SUITE(CmdRegistryTests)

BOOST_AUTO_TEST_CASE(Add_FillsDefaultSchemasAndKeepsTheDescription)
{
	cmd::command_registry commands;
	auto plain = commands.add(make_command("plain"));
	BOOST_REQUIRE(plain.active());
	const auto* desc = commands.find("plain");
	BOOST_REQUIRE(desc != nullptr);
	BOOST_CHECK(desc->args_schema == cmd::no_args_schema());
	BOOST_CHECK(desc->result_schema == cmd::no_args_schema());
	BOOST_CHECK_EQUAL(desc->version, 1u);
	BOOST_CHECK(desc->risk == cmd::danger::none);
	BOOST_CHECK(!desc->mutates);
	BOOST_CHECK(!desc->long_running);
	BOOST_CHECK(desc->expose_as_tool);

	cmd::command_desc risky = make_command("risky");
	risky.version = 3;
	risky.summary = "Deletes things";
	risky.risk = cmd::danger::confirm;
	risky.mutates = true;
	risky.long_running = true;
	risky.expose_as_tool = false;
	auto reg = commands.add(std::move(risky));
	const auto* kept = commands.find("risky");
	BOOST_REQUIRE(kept != nullptr);
	BOOST_CHECK_EQUAL(kept->version, 3u);
	BOOST_CHECK_EQUAL(kept->summary, "Deletes things");
	BOOST_CHECK(kept->risk == cmd::danger::confirm);
	BOOST_CHECK(kept->mutates);
	BOOST_CHECK(kept->long_running);
	BOOST_CHECK(!kept->expose_as_tool);
}

BOOST_AUTO_TEST_CASE(Generation_CountsEveryRegistrationAndUnregistration)
{
	cmd::command_registry registry;
	const std::uint64_t start = registry.generation();
	std::optional<cmd::registration> first = registry.add(make_command("test.one"));
	BOOST_TEST(registry.generation() == start + 1);
	// A command registered again under the same id (a module reloaded) leaves the size as it was and still moves the count.
	const std::size_t size = registry.size();
	first.reset();
	std::optional<cmd::registration> again = registry.add(make_command("test.one"));
	BOOST_TEST(registry.size() == size);
	BOOST_TEST(registry.generation() == start + 3);
	// A refused registration changes nothing.
	std::optional<cmd::registration> refused = registry.add(make_command("test.one"));
	BOOST_TEST(registry.generation() == start + 3);
}

BOOST_AUTO_TEST_CASE(Add_RefusesIncompleteAndDuplicateCommands)
{
	cmd::command_registry commands;
	auto first = commands.add(make_command("one"));
	BOOST_REQUIRE(first.active());

	BOOST_CHECK(!commands.add(make_command("")).active());
	cmd::command_desc no_body = make_command("no_body");
	no_body.invoke = nullptr;
	BOOST_CHECK(!commands.add(std::move(no_body)).active());

	BOOST_CHECK(!commands.add(make_command("one", [](const cmd::call&) { return cmd::result::failure("failed", "second"); })).active());
	// The first command is the one that stays.
	BOOST_CHECK_EQUAL(commands.size(), 1u);
	BOOST_CHECK(commands.invoke("one", json_of("{}")).ok);
}

BOOST_AUTO_TEST_CASE(Add_RefusesBadSchemasAndVersion)
{
	cmd::command_registry commands;
	std::vector<cmd::registration> kept;
	const auto refused = [&commands, &kept](const char* id, const char* args_schema, const char* result_schema, std::uint32_t version) {
		cmd::command_desc desc = make_command(id);
		if (args_schema) desc.args_schema = json_of(args_schema);
		if (result_schema) desc.result_schema = json_of(result_schema);
		desc.version = version;
		kept.push_back(commands.add(std::move(desc)));
		return !kept.back().active();
	};
	BOOST_CHECK(refused("bad_type", R"({"type":"nonsense"})", nullptr, 1));
	BOOST_CHECK(refused("unsupported_args", R"({"type":"object","pattern":"x"})", nullptr, 1));
	BOOST_CHECK(refused("unsupported_result", nullptr, R"({"type":"object","items":{}})", 1));
	BOOST_CHECK(refused("not_an_object", R"({"type":"string"})", nullptr, 1));
	BOOST_CHECK(refused("untyped", R"({"properties":{}})", nullptr, 1));
	BOOST_CHECK(refused("required_not_listed", R"({"type":"object","required":["a"]})", nullptr, 1));
	BOOST_CHECK(refused("version_zero", nullptr, nullptr, 0));
	BOOST_CHECK_EQUAL(commands.size(), 0u);

	BOOST_CHECK(!refused("fine", R"({"type":"object","properties":{"a":{"type":"string","minLength":1}},"required":["a"]})", nullptr, 2));
	BOOST_CHECK_EQUAL(commands.size(), 1u);
}

BOOST_AUTO_TEST_CASE(Registration_RemovesTheCommand)
{
	cmd::command_registry commands;
	{
		auto reg = commands.add(make_command("scoped"));
		BOOST_CHECK(reg.active());
		BOOST_CHECK(commands.contains("scoped"));
	}
	BOOST_CHECK(!commands.contains("scoped"));
	BOOST_CHECK(commands.find("scoped") == nullptr);
	BOOST_CHECK_EQUAL(commands.invoke("scoped", json_of("{}")).code, "not_found");

	auto reg = commands.add(make_command("again"));
	reg.reset();
	BOOST_CHECK(!reg.active());
	BOOST_CHECK(!commands.contains("again"));
	// The id is free again.
	auto reused = commands.add(make_command("again"));
	BOOST_CHECK(reused.active());
}

BOOST_AUTO_TEST_CASE(Registration_MovesWithoutRemovingAndAssignmentReleasesTheOldOne)
{
	cmd::command_registry commands;
	auto a = commands.add(make_command("a"));
	auto b = commands.add(make_command("b"));

	cmd::registration moved = std::move(a);
	BOOST_CHECK(!a.active());
	BOOST_CHECK(moved.active());
	BOOST_CHECK(commands.contains("a"));

	moved = std::move(b); // "a" goes away, "b" stays
	BOOST_CHECK(!commands.contains("a"));
	BOOST_CHECK(commands.contains("b"));
}

BOOST_AUTO_TEST_CASE(RegistrationOutlivingRegistry_IsHarmless)
{
	cmd::registration survivor;
	{
		cmd::command_registry commands;
		survivor = commands.add(make_command("gone"));
		BOOST_CHECK(survivor.active());
	}
	survivor.reset();
	BOOST_CHECK(!survivor.active());
}

BOOST_AUTO_TEST_CASE(CommandMayUnregisterItselfWhileRunning)
{
	cmd::command_registry commands;
	cmd::registration reg;
	reg = commands.add(make_command("self_removing", [&reg](const cmd::call&) {
		reg.reset();
		return cmd::result::success(boost::json::object{});
	}));
	BOOST_CHECK(commands.invoke("self_removing", json_of("{}")).ok);
	BOOST_CHECK(!commands.contains("self_removing"));
}

BOOST_AUTO_TEST_CASE(List_FollowsRegistrationOrder)
{
	cmd::command_registry commands;
	auto a = commands.add(make_command("b.first"));
	auto b = commands.add(make_command("a.second"));
	auto c = commands.add(make_command("c.third"));
	b.reset();
	auto d = commands.add(make_command("a.second"));

	std::vector<std::string> ids;
	for (const auto* desc : commands.list()) ids.push_back(desc->id);
	const std::vector<std::string> expected{ "b.first", "c.third", "a.second" };
	BOOST_TEST(ids == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(Invoke_ValidatesThenRunsAndReturnsTheResult)
{
	cmd::command_registry commands;
	int runs = 0;
	auto reg = commands.add(echo_command(runs));

	const auto outcome = commands.invoke("test.echo", json_of(R"({"text":"hi","times":2})"));
	BOOST_CHECK(outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "ok");
	BOOST_CHECK_EQUAL(boost::json::serialize(outcome.value), R"({"echo":"hi"})");
	BOOST_CHECK_EQUAL(runs, 1);
}

BOOST_AUTO_TEST_CASE(Invoke_RejectsInvalidArgumentsWithoutRunning)
{
	cmd::command_registry commands;
	int runs = 0;
	auto reg = commands.add(echo_command(runs));

	for (const char* text : { "{}", R"({"text":1})", R"({"text":""})", R"({"text":"x","times":0})", R"({"text":"x","extra":true})",
		"[]", "null", R"("text")" }) {
		BOOST_TEST_CONTEXT(text) {
			const auto outcome = commands.invoke("test.echo", json_of(text));
			BOOST_CHECK(!outcome.ok);
			BOOST_CHECK_EQUAL(outcome.code, "invalid_args");
			BOOST_CHECK(outcome.error.find("invalid arguments") != std::string::npos);
		}
	}
	BOOST_CHECK_EQUAL(runs, 0);
}

BOOST_AUTO_TEST_CASE(Invoke_FailsForMissingAndDisabledCommands)
{
	cmd::command_registry commands;
	BOOST_CHECK_EQUAL(commands.invoke("test.missing", json_of("{}")).code, "not_found");

	bool on = false;
	int runs = 0;
	cmd::command_desc desc = make_command("test.gated", [&runs](const cmd::call&) { ++runs; return cmd::result::success(); });
	desc.enabled = [&on] { return on; };
	auto reg = commands.add(std::move(desc));

	BOOST_CHECK(!commands.is_enabled("test.gated"));
	auto outcome = commands.invoke("test.gated", json_of("{}"));
	BOOST_CHECK(!outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "disabled");
	BOOST_CHECK_EQUAL(runs, 0);

	on = true;
	BOOST_CHECK(commands.is_enabled("test.gated"));
	BOOST_CHECK(commands.invoke("test.gated", json_of("{}")).ok);
	BOOST_CHECK_EQUAL(runs, 1);
	BOOST_CHECK(!commands.is_enabled("test.unknown"));
}

BOOST_AUTO_TEST_CASE(Invoke_PassesOnWhatTheCommandReturns)
{
	cmd::command_registry commands;
	auto failing = commands.add(make_command("test.failing", [](const cmd::call&) { return cmd::result::failure("failed", "nope"); }));
	auto asking = commands.add(make_command("test.asking", [](const cmd::call&) { return cmd::result::failure("needs_input", "discard_unsaved?"); }));

	const auto failed = commands.invoke("test.failing", json_of("{}"));
	BOOST_CHECK(!failed.ok);
	BOOST_CHECK_EQUAL(failed.code, "failed");
	BOOST_CHECK_EQUAL(failed.error, "nope");
	const auto asked = commands.invoke("test.asking", json_of("{}"));
	BOOST_CHECK_EQUAL(asked.code, "needs_input");
}

BOOST_AUTO_TEST_CASE(Invoke_RejectsAResultThatBreaksItsSchema)
{
	cmd::command_registry commands;
	cmd::command_desc desc = make_command("test.liar", [](const cmd::call&) { return cmd::result::success(); });
	desc.result_schema = json_of(R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})");
	auto reg = commands.add(std::move(desc));

	const auto outcome = commands.invoke("test.liar", json_of("{}"));
	BOOST_CHECK(!outcome.ok);
	BOOST_CHECK_EQUAL(outcome.code, "result_invalid");
	BOOST_CHECK(outcome.error.find("malformed result") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Invoke_PassesTheOriginAndGenerationToTheCommand)
{
	cmd::command_registry commands;
	std::vector<cmd::origin> seen;
	std::vector<std::uint64_t> generations;
	auto reg = commands.add(make_command("test.who", [&](const cmd::call& request) {
		seen.push_back(request.from);
		generations.push_back(request.expected_generation);
		return cmd::result::success();
	}));

	commands.invoke("test.who", json_of("{}")); // the json overload defaults to the UI
	commands.invoke("test.who", json_of("{}"), cmd::origin::test());
	commands.invoke("test.who", json_of("{}"), cmd::origin::agent("session-7"));
	commands.invoke("test.who", json_of("{}"), cmd::origin::game());
	cmd::call request;
	request.from = cmd::origin::agent("session-8");
	request.expected_generation = 42;
	commands.invoke("test.who", request);

	BOOST_REQUIRE_EQUAL(seen.size(), 5u);
	BOOST_CHECK(seen[0].kind == cmd::origin_kind::ui);
	BOOST_CHECK(seen[1].kind == cmd::origin_kind::test);
	BOOST_CHECK(seen[2].kind == cmd::origin_kind::agent);
	BOOST_CHECK_EQUAL(seen[2].session, "session-7");
	BOOST_CHECK(seen[3].kind == cmd::origin_kind::game);
	BOOST_CHECK(seen[4] == cmd::origin::agent("session-8"));
	BOOST_CHECK(seen[4] != seen[2]);
	BOOST_CHECK_EQUAL(generations[0], 0u);
	BOOST_CHECK_EQUAL(generations[4], 42u);
}

namespace {

// Asked about every agent call; refuses all of them.
struct counting_deny_gate final : cmd::policy_gate
{
	int asked = 0;
	cmd::policy_decision decide(const cmd::command_desc&, const cmd::call&) override
	{
		++asked;
		return cmd::policy_decision::deny("no");
	}
};

}

BOOST_AUTO_TEST_CASE(Headless_ACommandThatNeedsADisplay_IsUnavailableToEveryCaller_BeforeTheSchemaAndTheGate)
{
	cmd::command_registry commands;
	int runs = 0;
	cmd::command_desc capture = echo_command(runs);
	capture.needs_display = true;
	auto reg = commands.add(std::move(capture));
	auto gate = std::make_shared<counting_deny_gate>();
	commands.set_policy_gate(gate);
	commands.set_headless(true);
	BOOST_TEST(commands.headless());

	for (const cmd::origin& from : { cmd::origin::ui(), cmd::origin::test(), cmd::origin::agent("s1"), cmd::origin::game() }) {
		// Arguments that break the schema: the answer is still unavailable_headless.
		const cmd::result result = commands.invoke("test.echo", json_of(R"({"nope":1})"), from);
		BOOST_TEST(!result.ok);
		BOOST_TEST(result.code == cmd::codes::unavailable_headless);
	}
	BOOST_TEST(gate->asked == 0);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(Headless_OtherCommandsRun_AndWithADisplayNothingChanges)
{
	cmd::command_registry commands;
	int runs = 0;
	cmd::command_desc capture = echo_command(runs);
	capture.needs_display = true;
	auto reg = commands.add(std::move(capture));
	auto plain = commands.add(make_command("test.plain"));

	BOOST_TEST(!commands.headless());
	BOOST_TEST(commands.invoke("test.echo", json_of(R"({"text":"a"})")).ok);
	BOOST_TEST(commands.invoke("test.echo", json_of(R"({"nope":1})")).code == cmd::codes::invalid_args);

	commands.set_headless(true);
	BOOST_TEST(commands.invoke("test.plain", json_of("{}")).ok);
	commands.set_headless(false);
	BOOST_TEST(commands.invoke("test.echo", json_of(R"({"text":"b"})")).ok);
	BOOST_TEST(runs == 2);
}

// #681: the agent argument check runs for agents only, after the gate and before the schema, and without a gate too.
BOOST_AUTO_TEST_CASE(AgentArgumentCheck_RefusesOnlyAgents_BeforeTheSchema_WithOrWithoutAGate)
{
	cmd::command_registry commands;
	int runs = 0;
	auto reg = commands.add(echo_command(runs));
	int checked = 0;
	commands.set_agent_argument_check([&checked](const cmd::command_desc& command, const cmd::call& request) -> std::optional<cmd::result> {
		++checked;
		BOOST_TEST(command.id == "test.echo");
		if (request.args.as_object().contains("nope")) return cmd::result::failure(cmd::codes::denied, "refused by the check");
		return std::nullopt;
	});

	const cmd::result refused = commands.invoke("test.echo", json_of(R"({"nope":1})"), cmd::origin::agent("s1"));
	BOOST_TEST(refused.code == cmd::codes::denied); // not invalid_args: the check comes before the schema
	BOOST_TEST(refused.error == "refused by the check");
	BOOST_TEST(commands.invoke("test.echo", json_of(R"({"text":"a"})"), cmd::origin::agent("s1")).ok);
	BOOST_TEST(checked == 2);

	for (const cmd::origin& from : { cmd::origin::ui(), cmd::origin::test(), cmd::origin::game() }) {
		BOOST_TEST(commands.invoke("test.echo", json_of(R"({"nope":1})"), from).code == cmd::codes::invalid_args);
	}
	BOOST_TEST(checked == 2);

	commands.set_agent_argument_check(nullptr);
	BOOST_TEST(commands.invoke("test.echo", json_of(R"({"nope":1})"), cmd::origin::agent("s1")).code == cmd::codes::invalid_args);
	BOOST_TEST(runs == 1);
}

// #827: a module builds its command's schemas with any resource of its own; the registry keeps them in the default one.
BOOST_AUTO_TEST_CASE(Add_KeepsTheSchemasInTheDefaultResource)
{
	cmd::command_registry commands;
	cmd::registration added;
	{
		boost::json::monotonic_resource arena;
		// Assignment would copy into the field's resource: the schemas are constructed in place, as a module's desc has them.
		const auto construct_in_arena = [&arena](boost::json::value& field, std::string_view text) {
			std::destroy_at(&field);
			std::construct_at(&field, boost::json::parse(text, boost::json::storage_ptr(&arena)));
		};
		cmd::command_desc desc = make_command("test.arena");
		construct_in_arena(desc.args_schema, R"({"type":"object","properties":{"text":{"type":"string"}},"additionalProperties":false})");
		construct_in_arena(desc.result_schema, R"({"type":"object","properties":{},"additionalProperties":false})");
		BOOST_REQUIRE((desc.args_schema.storage().get() != boost::json::storage_ptr().get()));
		added = commands.add(std::move(desc));
	}
	BOOST_REQUIRE(added.active());
	const auto* kept = commands.find("test.arena");
	BOOST_REQUIRE(kept != nullptr);
	BOOST_TEST((kept->args_schema.storage().get() == boost::json::storage_ptr().get()));
	BOOST_TEST((kept->result_schema.storage().get() == boost::json::storage_ptr().get()));
	BOOST_TEST(kept->args_schema.at("properties").at("text").at("type").as_string() == "string");
	BOOST_TEST(commands.invoke("test.arena", json_of(R"({"text":"a"})"), cmd::origin::test()).ok);
}

// #827: what a command answers outlives the command (a future of main_thread_queue, a module unloaded after it ran).
BOOST_AUTO_TEST_CASE(Invoke_KeepsTheResultPastTheCommandsResource)
{
	cmd::command_registry commands;
	std::optional<cmd::result> outcome;
	{
		boost::json::monotonic_resource arena;
		cmd::command_desc desc = make_command("test.arena_result", [&arena](const cmd::call&) {
			return cmd::result::success(boost::json::parse(R"({"text":"a text long enough to be allocated in the arena"})",
				boost::json::storage_ptr(&arena)));
		});
		desc.result_schema = json_of(R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})");
		auto added = commands.add(std::move(desc));
		BOOST_REQUIRE(added.active());
		// Constructed from the answer, not assigned: assignment would copy into the optional's own resource.
		outcome.emplace(commands.invoke("test.arena_result", boost::json::object{}, cmd::origin::test()));
	}
	BOOST_REQUIRE(outcome->ok);
	BOOST_TEST((outcome->value.storage().get() == boost::json::storage_ptr().get()));
	BOOST_TEST(outcome->value.at("text").as_string() == "a text long enough to be allocated in the arena");
}

BOOST_AUTO_TEST_SUITE_END()
