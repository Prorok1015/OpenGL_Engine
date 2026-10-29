#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/logger/engine_log_sinks.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The agent policy (A6b #293 / A6c #449): defaults, the two layers and "the stricter wins", the files, and the gate in
// a real command_registry.
namespace {

namespace fs = std::filesystem;
using cmd::agent_category;
using cmd::policy_mode;

cmd::command_desc command_of(std::string id, agent_category category, cmd::danger risk = cmd::danger::none)
{
	cmd::command_desc desc;
	desc.id = std::move(id);
	desc.category = category;
	desc.risk = risk;
	desc.summary = "does " + desc.id;
	desc.invoke = [](const cmd::call&) { return cmd::result::success(); };
	return desc;
}

edt::policy_layer layer_with(agent_category category, policy_mode mode)
{
	edt::policy_layer layer;
	layer.categories[category] = mode;
	return layer;
}

edt::policy_layer layer_with(std::string command, policy_mode mode)
{
	edt::policy_layer layer;
	layer.commands[std::move(command)] = mode;
	return layer;
}

}

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyDefaults)

BOOST_AUTO_TEST_CASE(TheOwnersTable_ForEveryCategory)
{
	BOOST_TEST((edt::default_mode(agent_category::read) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::scene_edit) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::code_edit) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::code_files) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(agent_category::cmake_presets) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(agent_category::build) == policy_mode::deny));
	BOOST_TEST((edt::default_mode(agent_category::live) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::unset) == policy_mode::deny));
	BOOST_TEST((edt::default_mode(agent_category::not_for_agents) == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(Risk_OnlyTightensTheCategorysDefault)
{
	BOOST_TEST((edt::default_mode(command_of("a.b", agent_category::read, cmd::danger::confirm)) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(command_of("a.b", agent_category::read, cmd::danger::deny_by_default)) == policy_mode::deny));
	BOOST_TEST((edt::default_mode(command_of("a.b", agent_category::code_files, cmd::danger::confirm)) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(command_of("a.b", agent_category::build, cmd::danger::none)) == policy_mode::deny));
	BOOST_TEST((edt::default_mode(command_of("a.b", agent_category::scene_edit, cmd::danger::none)) == policy_mode::allow));
}

BOOST_AUTO_TEST_CASE(AnUnclassifiedCommand_IsNeverAvailable_EvenIfAFileAllowsIt)
{
	const auto unset = command_of("x.y", agent_category::unset);
	const auto not_for = command_of("x.z", agent_category::not_for_agents);
	edt::policy_layer allow_everything = layer_with("x.y", policy_mode::allow);
	allow_everything.commands["x.z"] = policy_mode::allow;

	BOOST_TEST((edt::effective_mode(unset, allow_everything, {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(not_for, allow_everything, {}).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyLayers)

BOOST_AUTO_TEST_CASE(ACommandsOwnEntryBeatsItsCategory)
{
	edt::policy_layer layer = layer_with(agent_category::code_files, policy_mode::allow);
	layer.commands["code.delete"] = policy_mode::deny;

	BOOST_TEST((layer.resolve(command_of("code.create", agent_category::code_files)) == policy_mode::allow));
	BOOST_TEST((layer.resolve(command_of("code.delete", agent_category::code_files)) == policy_mode::deny));
	BOOST_TEST(!layer.resolve(command_of("scene.x", agent_category::scene_edit)).has_value());
}

BOOST_AUTO_TEST_CASE(Merge_IsTheStricterOfProjectAndPersonal_ForEveryCombination)
{
	const policy_mode modes[] = { policy_mode::allow, policy_mode::ask, policy_mode::deny };
	const auto command = command_of("scene.move", agent_category::scene_edit);

	for (const policy_mode project : modes) {
		for (const policy_mode personal : modes) {
			BOOST_TEST_INFO(cmd::to_string(project) << " + " << cmd::to_string(personal));
			const auto effective = edt::effective_mode(command, layer_with(agent_category::scene_edit, project),
				layer_with(agent_category::scene_edit, personal));
			BOOST_TEST((effective.mode == cmd::strictest(project, personal)));
		}
	}
}

BOOST_AUTO_TEST_CASE(Merge_WithoutAPersonalEntry_TheProjectOrTheDefaultStands)
{
	const auto command = command_of("code.create", agent_category::code_files);

	BOOST_TEST((edt::effective_mode(command, {}, {}).mode == policy_mode::ask));
	BOOST_TEST((edt::effective_mode(command, {}, {}).source == edt::policy_source::default_table));
	BOOST_TEST((edt::effective_mode(command, layer_with(agent_category::code_files, policy_mode::allow), {}).mode == policy_mode::allow));
	BOOST_TEST((edt::effective_mode(command, layer_with(agent_category::code_files, policy_mode::allow), {}).source == edt::policy_source::project));
	BOOST_TEST((edt::effective_mode(command, layer_with(agent_category::code_files, policy_mode::deny), {}).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(APersonalAllow_NeverLiftsAProjectAskOrDeny_NorADefaultDeny)
{
	const auto build = command_of("build.game_module", agent_category::build, cmd::danger::deny_by_default);
	const auto files = command_of("code.create", agent_category::code_files);

	BOOST_TEST((edt::effective_mode(build, {}, layer_with("build.game_module", policy_mode::allow)).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(files, layer_with(agent_category::code_files, policy_mode::ask), layer_with("code.create", policy_mode::allow)).mode
		== policy_mode::ask));
	BOOST_TEST((edt::effective_mode(files, layer_with(agent_category::code_files, policy_mode::deny), layer_with(agent_category::code_files, policy_mode::allow)).mode
		== policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(APersonalDeny_TightensWhateverTheProjectAllowed_AndSaysWhy)
{
	const auto files = command_of("code.create", agent_category::code_files);

	const auto effective = edt::effective_mode(files, layer_with(agent_category::code_files, policy_mode::allow),
		layer_with(agent_category::code_files, policy_mode::deny));

	BOOST_TEST((effective.mode == policy_mode::deny));
	BOOST_TEST((effective.source == edt::policy_source::personal));
	BOOST_TEST(!effective.reason.empty());
}

BOOST_AUTO_TEST_CASE(AProjectCanLoosenADefault_ForExample_AllowBuilds)
{
	const auto build = command_of("build.game_module", agent_category::build, cmd::danger::deny_by_default);

	BOOST_TEST((edt::effective_mode(build, layer_with("build.game_module", policy_mode::allow), {}).mode == policy_mode::allow));
	BOOST_TEST((edt::effective_mode(build, layer_with(agent_category::build, policy_mode::ask), {}).mode == policy_mode::ask));
}

BOOST_AUTO_TEST_CASE(ABrokenLayer_DeniesEverything_ReadsIncluded)
{
	const auto read = command_of("scene.query", agent_category::read);
	const auto edit = command_of("scene.move", agent_category::scene_edit);

	BOOST_TEST((edt::effective_mode(read, {}, {}, "bad json", {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(edit, {}, {}, "bad json", {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(read, {}, {}, {}, "bad json").mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(edit, {}, {}, {}, "bad json").mode == policy_mode::deny));
	BOOST_TEST(edt::effective_mode(edit, {}, {}, "bad json", {}).reason.find("bad json") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(LiveCommands_AreDeniedWhileLiveModeIsOff_WhateverAnyLayerSays)
{
	const auto live = command_of("live.status", agent_category::live);
	edt::policy_layer allow_all = layer_with(agent_category::live, policy_mode::allow);
	allow_all.commands["live.status"] = policy_mode::allow;

	BOOST_TEST((edt::effective_mode(live, allow_all, allow_all, {}, {}, false).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(live, {}, {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(live, {}, {}, {}, {}, true).mode == policy_mode::allow));
	BOOST_TEST((edt::effective_mode(live, layer_with(agent_category::live, policy_mode::deny), {}, {}, {}, true).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(ACategoryEntry_DoesNotLiftTheRiskFloor_OnlyTheCommandsOwnEntryDoes)
{
	const auto build = command_of("build.game_module", agent_category::build, cmd::danger::deny_by_default);
	const auto confirm = command_of("code.wipe", agent_category::code_edit, cmd::danger::confirm);

	BOOST_TEST((edt::effective_mode(build, layer_with(agent_category::build, policy_mode::allow), {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode(build, layer_with("build.game_module", policy_mode::allow), {}).mode == policy_mode::allow));
	BOOST_TEST((edt::effective_mode(confirm, layer_with(agent_category::code_edit, policy_mode::allow), {}).mode == policy_mode::ask));
	BOOST_TEST((edt::effective_mode(confirm, layer_with("code.wipe", policy_mode::allow), {}).mode == policy_mode::allow));
	BOOST_TEST((edt::effective_mode(confirm, layer_with(agent_category::code_edit, policy_mode::deny), {}).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyFile)

BOOST_AUTO_TEST_CASE(Parse_TheMapsAndTheLists)
{
	const auto parsed = edt::parse_policy_layer(R"({"version":1,"categories":{"code_files":"allow","build":"ask"},
		"commands":{"code.delete":"deny"},"allow":["asset.import"],"deny":["x.y"]})");

	BOOST_TEST(parsed.error.empty());
	BOOST_TEST((parsed.layer.categories.at(agent_category::code_files) == policy_mode::allow));
	BOOST_TEST((parsed.layer.categories.at(agent_category::build) == policy_mode::ask));
	BOOST_TEST((parsed.layer.commands.at("code.delete") == policy_mode::deny));
	BOOST_TEST((parsed.layer.commands.at("asset.import") == policy_mode::allow));
	BOOST_TEST((parsed.layer.commands.at("x.y") == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(Parse_ACommandInTwoPlaces_GetsTheStricterMode)
{
	const auto parsed = edt::parse_policy_layer(R"({"commands":{"a.b":"allow"},"deny":["a.b"]})");
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST((parsed.layer.commands.at("a.b") == policy_mode::deny));

	const auto reversed = edt::parse_policy_layer(R"({"commands":{"a.b":"deny"},"allow":["a.b"]})");
	BOOST_TEST((reversed.layer.commands.at("a.b") == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(Parse_AnEmptyObject_IsAnEmptyLayer)
{
	const auto parsed = edt::parse_policy_layer("{}");
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST(parsed.layer.empty());
}

BOOST_AUTO_TEST_CASE(Parse_Mistakes_AreErrorsNotSkipped)
{
	for (const char* bad : {
		"", "[]", "not json", R"({"version":2})", R"({"version":"1"})", R"({"categories":[]})", R"({"categories":{"code_file":"allow"}})",
		R"({"categories":{"code_files":"yes"}})", R"({"categories":{"code_files":1}})", R"({"categories":{"unset":"allow"}})",
		R"({"categories":{"not_for_agents":"allow"}})", R"({"commands":{"a.b":"maybe"}})", R"({"commands":[]})", R"({"allow":"a.b"})",
		R"({"allow":[1]})", R"({"alow":["a.b"]})" }) {
		BOOST_TEST_INFO(bad);
		const auto parsed = edt::parse_policy_layer(bad);
		BOOST_TEST(!parsed.error.empty());
		BOOST_TEST(parsed.layer.empty());
	}
}

BOOST_AUTO_TEST_CASE(Parse_ARepeatedKey_IsAnError_NotTheLastOneWinning)
{
	for (const char* bad : {
		R"({"categories":{"code_edit":"deny","code_edit":"allow"}})",
		R"({"commands":{"a.b":"deny","a.b":"allow"}})",
		R"({"version":1,"version":1})",
		R"({"categories":{},"categories":{}})",
		R"({"allow":["a.b"],"allow":["c.d"]})" }) {
		BOOST_TEST_INFO(bad);
		const auto parsed = edt::parse_policy_layer(bad);
		BOOST_TEST(!parsed.error.empty());
		BOOST_TEST(parsed.error.find("twice") != std::string::npos);
		BOOST_TEST(parsed.layer.empty());
	}
	// The same key in two different objects is fine.
	BOOST_TEST(edt::parse_policy_layer(R"({"categories":{"read":"allow"},"commands":{"read":"allow"}})").error.empty());
}

BOOST_AUTO_TEST_CASE(Serialize_RoundTrips)
{
	edt::policy_layer layer = layer_with(agent_category::code_files, policy_mode::allow);
	layer.categories[agent_category::build] = policy_mode::ask;
	layer.commands["code.delete"] = policy_mode::deny;

	const auto parsed = edt::parse_policy_layer(edt::serialize_policy_layer(layer));

	BOOST_TEST(parsed.error.empty());
	BOOST_TEST((parsed.layer.categories == layer.categories));
	BOOST_TEST((parsed.layer.commands == layer.commands));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyStore)

BOOST_AUTO_TEST_CASE(NoFiles_TheDefaultsApply)
{
	test_util::temp_dir dir("edt_agent_policy_none");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };

	// No folder, no file: a fresh project. Not an error on any platform, so the defaults apply, reads included.
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::allow));
	BOOST_TEST((policy.effective(command_of("scene.move", agent_category::scene_edit)).mode == policy_mode::allow));
	const auto effective = policy.effective(command_of("code.create", agent_category::code_files));

	BOOST_TEST((effective.mode == policy_mode::ask));
	BOOST_TEST(policy.project_error().empty());
	BOOST_TEST(policy.personal_error().empty());
}

BOOST_AUTO_TEST_CASE(AFileEditedByAPerson_IsTakenOverWithoutARestart)
{
	test_util::temp_dir dir("edt_agent_policy_edit");
	const fs::path project_file = dir.path() / ".snk" / "agent_policy.json";
	edt::agent_policy policy{ project_file, dir.path() / "personal.json" };
	const auto command = command_of("code.create", agent_category::code_files);
	BOOST_TEST((policy.effective(command).mode == policy_mode::ask));

	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"allow"}})");
	BOOST_TEST((policy.effective(command).mode == policy_mode::allow));

	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"deny","extra":1}})");
	BOOST_TEST((policy.effective(command).mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}

BOOST_AUTO_TEST_CASE(ABrokenFile_FailsClosed_UntilItIsFixed)
{
	test_util::temp_dir dir("edt_agent_policy_broken");
	dir.write(".snk/agent_policy.json", "{ not json");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };

	BOOST_TEST((policy.effective(command_of("scene.move", agent_category::scene_edit)).mode == policy_mode::deny));
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));

	dir.write(".snk/agent_policy.json", "{}");
	BOOST_TEST((policy.effective(command_of("scene.move", agent_category::scene_edit)).mode == policy_mode::allow));
}

BOOST_AUTO_TEST_CASE(SaveProject_WritesTheFileAndTakesTheLayerOver)
{
	test_util::temp_dir dir("edt_agent_policy_save");
	const fs::path project_file = dir.path() / ".snk" / "agent_policy.json";
	edt::agent_policy policy{ project_file, dir.path() / "personal.json" };

	BOOST_TEST(!policy.save_project(layer_with(agent_category::code_files, policy_mode::allow)).has_value());

	BOOST_TEST(fs::exists(project_file));
	BOOST_TEST((policy.effective(command_of("code.create", agent_category::code_files)).mode == policy_mode::allow));
	edt::agent_policy again{ project_file, dir.path() / "personal.json" };
	BOOST_TEST((again.effective(command_of("code.create", agent_category::code_files)).mode == policy_mode::allow));
}

BOOST_AUTO_TEST_CASE(SaveProject_WithAFailingWriter_ReportsAndKeepsTheOldLayer)
{
	test_util::temp_dir dir("edt_agent_policy_save_fail");
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"ask"}})");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	policy.refresh();

	const auto error = policy.save_project(layer_with(agent_category::code_files, policy_mode::allow),
		[](const fs::path&, std::string_view) { return std::optional<std::string>{ "disk full" }; });

	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(*error == "disk full");
	BOOST_TEST((policy.effective(command_of("code.create", agent_category::code_files)).mode == policy_mode::ask));
}

BOOST_AUTO_TEST_CASE(ThePersonalFile_OnlyTightens)
{
	test_util::temp_dir dir("edt_agent_policy_personal");
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"allow","build":"deny"}})");
	dir.write("personal.json", R"({"categories":{"code_files":"deny","build":"allow"}})");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };

	// The personal deny tightens the project's allow; the personal allow does not lift the project's deny.
	BOOST_TEST((policy.effective(command_of("code.create", agent_category::code_files)).mode == policy_mode::deny));
	BOOST_TEST((policy.effective(command_of("build.run", agent_category::build)).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(LiveCommands_FollowTheLiveModeTheOwnerGives)
{
	test_util::temp_dir dir("edt_agent_policy_live");
	dir.write(".snk/agent_policy.json", R"({"categories":{"live":"allow"}})");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	const auto live = command_of("live.status", agent_category::live);

	BOOST_TEST((policy.effective(live).mode == policy_mode::deny));
	bool on = false;
	policy.set_live_mode([&on] { return on; });
	BOOST_TEST((policy.effective(live).mode == policy_mode::deny));
	on = true;
	BOOST_TEST((policy.effective(live).mode == policy_mode::allow));
	on = false;
	BOOST_TEST((policy.effective(live).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(ABrokenFile_DeniesEvenReads)
{
	test_util::temp_dir dir("edt_agent_policy_broken_reads");
	dir.write(".snk/agent_policy.json", R"({"commands":{"scene.query":"deny"},"commands":{}})");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };

	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(ACommandIdNoCommandHas_IsWarnedAboutOncePerLoad)
{
	test_util::temp_dir dir("edt_agent_policy_unknown");
	dir.write(".snk/agent_policy.json", R"({"deny":["no.such_command","scene.query"],"allow":["another.typo"]})");
	std::vector<std::string> warnings;
	const auto sink = engine::add_log_sink([&warnings](std::string_view category, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn && category == "editor/agent_policy") warnings.emplace_back(message);
	});
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	policy.set_known_commands([](std::string_view id) { return id == "scene.query"; });
	const auto read = command_of("scene.query", agent_category::read);

	policy.effective(read);
	policy.effective(read);

	// Both unknown ids once, though effective() ran twice, and the known one not at all; the layer still works.
	BOOST_TEST(std::ranges::count_if(warnings, [](const std::string& w) { return w.find("no.such_command") != std::string::npos; }) == 1);
	BOOST_TEST(std::ranges::count_if(warnings, [](const std::string& w) { return w.find("another.typo") != std::string::npos; }) == 1);
	BOOST_TEST(std::ranges::count_if(warnings, [](const std::string& w) { return w.find("'scene.query'") != std::string::npos; }) == 0);
	BOOST_TEST((policy.effective(read).mode == policy_mode::deny));
}

#ifndef _WIN32
BOOST_AUTO_TEST_CASE(AFileThatCannotBeExamined_IsALayerError_NotNoFile)
{
	test_util::temp_dir dir("edt_agent_policy_stat");
	dir.write("locked/agent_policy.json", R"({"categories":{"scene_edit":"allow"}})");
	const fs::path locked = dir.path() / "locked";
	fs::permissions(locked, fs::perms::none);
	std::error_code ec;
	fs::status(locked / "agent_policy.json", ec);
	if (!ec) {
		fs::permissions(locked, fs::perms::owner_all);
		BOOST_TEST_MESSAGE("permissions are not enforced here (running as root?)");
		return;
	}

	edt::agent_policy policy{ locked / "agent_policy.json", dir.path() / "personal.json" };
	const auto decision = policy.effective(command_of("scene.query", agent_category::read));
	fs::permissions(locked, fs::perms::owner_all);

	BOOST_TEST((decision.mode == policy_mode::deny));
	BOOST_TEST(!policy.project_error().empty());
}
#endif

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyGate)

namespace {

struct gate_fixture
{
	test_util::temp_dir dir{ "edt_agent_policy_gate" };
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	std::shared_ptr<edt::agent_policy_gate> gate = std::make_shared<edt::agent_policy_gate>(policy);
	cmd::command_registry commands;
	int runs = 0;
	std::vector<cmd::registration> registrations;

	gate_fixture()
	{
		add("scene.move", agent_category::scene_edit);
		add("code.create", agent_category::code_files);
		add("build.game_module", agent_category::build, cmd::danger::deny_by_default);
		add("misc.thing", agent_category::unset);
		commands.set_policy_gate(gate);
	}

	void add(std::string id, agent_category category, cmd::danger risk = cmd::danger::none)
	{
		cmd::command_desc desc = command_of(std::move(id), category, risk);
		desc.invoke = [this](const cmd::call&) {
			++runs;
			return cmd::result::success();
		};
		registrations.push_back(commands.add(std::move(desc)));
	}

	cmd::result call(const std::string& id, cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, boost::json::object{}, std::move(from));
	}
};

}

BOOST_FIXTURE_TEST_CASE(AnAgentCall_FollowsTheDefaults, gate_fixture)
{
	gate->set_can_ask(true);

	BOOST_TEST(call("scene.move").ok);
	BOOST_TEST(call("code.create").code == cmd::codes::confirmation_required);
	BOOST_TEST(call("build.game_module").code == cmd::codes::denied);
	BOOST_TEST(call("misc.thing").code == cmd::codes::denied);
	BOOST_TEST(runs == 1);
}

BOOST_FIXTURE_TEST_CASE(WithoutAPersonToAsk_AskIsDenied, gate_fixture)
{
	BOOST_TEST(call("code.create").code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(ThePeopleAndTestsAreNotAskedAbout, gate_fixture)
{
	BOOST_TEST(call("build.game_module", cmd::origin::ui()).ok);
	BOOST_TEST(call("misc.thing", cmd::origin::test()).ok);
	BOOST_TEST(runs == 2);
}

BOOST_FIXTURE_TEST_CASE(AnApprovedAsk_RunsOnceThroughTheRealPolicy, gate_fixture)
{
	gate->set_can_ask(true);
	const auto asked = call("code.create");
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	const std::string ticket = std::string{ asked.value.as_object().at("ticket").as_string().c_str() };
	BOOST_TEST(commands.approve(ticket));

	cmd::call again;
	again.args = boost::json::object{};
	again.from = cmd::origin::agent("s1");
	again.confirmation = ticket;
	BOOST_TEST(commands.invoke("code.create", again).ok);
	BOOST_TEST(runs == 1);
}

BOOST_FIXTURE_TEST_CASE(AFileThatTurnsToDeny_StopsTheCallAtOnce, gate_fixture)
{
	BOOST_TEST(call("scene.move").ok);

	dir.write(".snk/agent_policy.json", R"({"categories":{"scene_edit":"deny"}})");

	BOOST_TEST(call("scene.move").code == cmd::codes::denied);
	BOOST_TEST(runs == 1);
}

BOOST_AUTO_TEST_SUITE_END()
