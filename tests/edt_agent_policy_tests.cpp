#include <boost/test/unit_test.hpp>
#include "edt_agent_policy.h"
#include <snk/command/cmd_registry.h>
#include <boost/json/parse.hpp>
#include <snk/common/logger/engine_log_sinks.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <functional>
#include <initializer_list>
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
	BOOST_TEST((edt::default_mode(agent_category::code_edit) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(agent_category::code_files) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(agent_category::cmake_presets) == policy_mode::ask));
	BOOST_TEST((edt::default_mode(agent_category::build) == policy_mode::deny));
	BOOST_TEST((edt::default_mode(agent_category::live) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::editor_control) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::vcs_read) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::vcs_commit) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::vcs_branch) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::vcs_sync) == policy_mode::allow));
	BOOST_TEST((edt::default_mode(agent_category::vcs_push) == policy_mode::allow));
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
	BOOST_TEST((edt::effective_mode(build, layer_with("build.game_module", policy_mode::ask), {}).mode == policy_mode::ask));
	// An entry for the whole category is held to the command's risk floor (deny_by_default): it cannot loosen it.
	BOOST_TEST((edt::effective_mode(build, layer_with(agent_category::build, policy_mode::ask), {}).mode == policy_mode::deny));
	// A category entry still works for a command without a risk of its own.
	const auto plain_build = command_of("build.plain", agent_category::build);
	BOOST_TEST((edt::effective_mode(plain_build, layer_with(agent_category::build, policy_mode::ask), {}).mode == policy_mode::ask));
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

BOOST_AUTO_TEST_SUITE(EdtAgentPolicyVcs)

BOOST_AUTO_TEST_CASE(TheVcsCategories_AreSettableByName)
{
	for (const auto category : { agent_category::vcs_read, agent_category::vcs_commit, agent_category::vcs_branch,
			agent_category::vcs_sync, agent_category::vcs_push }) {
		BOOST_TEST_CONTEXT(cmd::to_string(category)) {
			BOOST_TEST((edt::parse_settable_category(cmd::to_string(category)) == category));
		}
	}
	BOOST_TEST(std::string_view(cmd::to_string(agent_category::vcs_push)) == "vcs_push");
	const auto parsed = edt::parse_policy_layer(R"({"categories":{"vcs_push":"ask","vcs_sync":"deny"}})");
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST((parsed.layer.categories.at(agent_category::vcs_push) == policy_mode::ask));
}

BOOST_AUTO_TEST_CASE(TheVcsSection_ParsesAndRoundTrips)
{
	const auto parsed = edt::parse_policy_layer(R"({"version":1,"vcs":{"push_branches":["agent/*","bot/*"],"remotes":["origin"],
		"protected_branches":["release/*"],"allow_hooks":true,"allow_filters":false,"allow_repo_credential_helper":true,
		"allow_any_repo_config":false}})");
	BOOST_TEST(parsed.error.empty(), parsed.error);
	BOOST_REQUIRE(parsed.layer.vcs.has_value());
	const edt::vcs_policy_layer& vcs = *parsed.layer.vcs;
	BOOST_TEST((vcs.push_branches == std::vector<std::string>{ "agent/*", "bot/*" }));
	BOOST_TEST((vcs.remotes == std::vector<std::string>{ "origin" }));
	BOOST_TEST((vcs.protected_branches == std::vector<std::string>{ "release/*" }));
	BOOST_TEST((vcs.allow_hooks == true));
	BOOST_TEST((vcs.allow_filters == false));
	BOOST_TEST((vcs.allow_repo_credential_helper == true));
	BOOST_TEST((vcs.allow_any_repo_config == false));
	BOOST_TEST(!parsed.layer.empty());

	const auto again = edt::parse_policy_layer(edt::serialize_policy_layer(parsed.layer));
	BOOST_TEST(again.error.empty(), again.error);
	BOOST_TEST((again.layer.vcs == parsed.layer.vcs));

	// Only what is set is written back.
	edt::policy_layer partial;
	partial.vcs = edt::vcs_policy_layer{};
	partial.vcs->remotes = std::vector<std::string>{ "upstream" };
	const auto partial_again = edt::parse_policy_layer(edt::serialize_policy_layer(partial));
	BOOST_TEST(partial_again.error.empty(), partial_again.error);
	BOOST_TEST((partial_again.layer.vcs == partial.vcs));
	BOOST_TEST(!partial_again.layer.vcs->push_branches.has_value());

	// No section: none is written.
	BOOST_TEST(edt::serialize_policy_layer(edt::policy_layer{}).find("vcs") == std::string::npos);
	const auto empty_section = edt::parse_policy_layer(R"({"vcs":{}})");
	BOOST_TEST(empty_section.error.empty());
	BOOST_TEST(empty_section.layer.vcs.has_value());
}

BOOST_AUTO_TEST_CASE(TheVcsSection_Mistakes_AreErrorsNotSkipped)
{
	for (const char* bad : {
		R"({"vcs":[]})", R"({"vcs":{"push_branch":["agent/*"]}})", R"({"vcs":{"push_branches":"agent/*"}})",
		R"({"vcs":{"push_branches":[1]}})", R"({"vcs":{"remotes":[""]}})", R"({"vcs":{"protected_branches":{}}})",
		R"({"vcs":{"allow_hooks":"true"}})", R"({"vcs":{"allow_filters":1}})", R"({"vcs":{"allow_any_repo_config":null}})",
		R"({"vcs":{"remotes":["a"],"remotes":["b"]}})" }) {
		BOOST_TEST_INFO(bad);
		const auto parsed = edt::parse_policy_layer(bad);
		BOOST_TEST(!parsed.error.empty());
		BOOST_TEST(parsed.layer.empty());
	}
}

BOOST_AUTO_TEST_CASE(APersonalLayer_MaySwitchAFlagOff_NeverOn)
{
	for (const char* flag : { "allow_hooks", "allow_filters", "allow_repo_credential_helper", "allow_any_repo_config" }) {
		BOOST_TEST_CONTEXT(flag) {
			const std::string on = std::string(R"({"vcs":{")") + flag + R"(":true}})";
			const std::string off = std::string(R"({"vcs":{")") + flag + R"(":false}})";
			BOOST_TEST(edt::parse_policy_layer(on, edt::policy_layer_kind::project).error.empty());
			const auto refused = edt::parse_policy_layer(on, edt::policy_layer_kind::personal);
			BOOST_TEST(refused.error.find("only the project policy can allow") != std::string::npos, refused.error);
			BOOST_TEST(refused.layer.empty());
			BOOST_TEST(edt::parse_policy_layer(off, edt::policy_layer_kind::personal).error.empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(EffectiveVcs_WithoutFiles_IsTheDefault)
{
	const edt::vcs_policy vcs = edt::effective_vcs({}, {});
	BOOST_TEST((vcs.push_branches == std::vector<std::string>{ "agent/*" }));
	BOOST_TEST((vcs.remotes == std::vector<std::string>{ "origin" }));
	BOOST_TEST((vcs.protected_branches == std::vector<std::string>{ "main", "master" }));
	BOOST_TEST(!vcs.permissions.allow_hooks);
	BOOST_TEST(!vcs.permissions.allow_filters);
	BOOST_TEST(!vcs.permissions.allow_repo_credential_helper);
	BOOST_TEST(!vcs.permissions.allow_any_repo_config);
}

BOOST_AUTO_TEST_CASE(EffectiveVcs_ThePersonalLayerOnlyNarrows)
{
	edt::policy_layer project;
	project.vcs = edt::vcs_policy_layer{};
	project.vcs->push_branches = std::vector<std::string>{ "agent/*", "bot/*" };
	project.vcs->remotes = std::vector<std::string>{ "origin", "upstream" };
	project.vcs->protected_branches = std::vector<std::string>{ "release/*" };
	project.vcs->allow_hooks = true;
	project.vcs->allow_filters = true;
	project.vcs->allow_any_repo_config = true;
	edt::policy_layer personal;
	personal.vcs = edt::vcs_policy_layer{};
	personal.vcs->push_branches = std::vector<std::string>{ "bot/*", "everything/*" };
	personal.vcs->remotes = std::vector<std::string>{ "upstream" };
	personal.vcs->protected_branches = std::vector<std::string>{ "develop", "release/*" };
	personal.vcs->allow_filters = false;

	const edt::vcs_policy vcs = edt::effective_vcs(project, personal);
	// A pattern only the personal layer names is never added.
	BOOST_TEST((vcs.push_branches == std::vector<std::string>{ "bot/*" }));
	BOOST_TEST((vcs.remotes == std::vector<std::string>{ "upstream" }));
	BOOST_TEST((vcs.protected_branches == std::vector<std::string>{ "main", "master", "release/*", "develop" }));
	BOOST_TEST(vcs.permissions.allow_hooks);
	BOOST_TEST(!vcs.permissions.allow_filters);
	BOOST_TEST(!vcs.permissions.allow_repo_credential_helper);
	BOOST_TEST(vcs.permissions.allow_any_repo_config);

	// Without a project list the personal one narrows the default.
	edt::policy_layer narrowing;
	narrowing.vcs = edt::vcs_policy_layer{};
	narrowing.vcs->push_branches = std::vector<std::string>{ "agent/*", "bot/*" };
	narrowing.vcs->remotes = std::vector<std::string>{};
	const edt::vcs_policy narrowed = edt::effective_vcs({}, narrowing);
	BOOST_TEST((narrowed.push_branches == std::vector<std::string>{ "agent/*" }));
	BOOST_TEST(narrowed.remotes.empty());
}

BOOST_AUTO_TEST_CASE(EffectiveVcs_ABrokenLayer_IsTheStrictest)
{
	edt::policy_layer project;
	project.vcs = edt::vcs_policy_layer{};
	project.vcs->push_branches = std::vector<std::string>{ "*" };
	project.vcs->allow_hooks = true;
	const std::vector<std::pair<std::string, std::string>> errors{ { "bad", "" }, { "", "bad" } };
	for (const auto& error : errors) {
		BOOST_TEST_CONTEXT(error.first << "/" << error.second) {
			const edt::vcs_policy vcs = edt::effective_vcs(project, {}, error.first, error.second);
			BOOST_TEST(vcs.push_branches.empty());
			BOOST_TEST(vcs.remotes.empty());
			BOOST_TEST((vcs.protected_branches == std::vector<std::string>{ "main", "master" }));
			BOOST_TEST(!vcs.permissions.allow_hooks);
			BOOST_TEST(!vcs.permissions.allow_filters);
			BOOST_TEST(!vcs.permissions.allow_repo_credential_helper);
			BOOST_TEST(!vcs.permissions.allow_any_repo_config);
		}
	}
}

BOOST_AUTO_TEST_CASE(EffectiveVcs_FromTheFiles_ReadAgainWhenTheyChange)
{
	test_util::temp_dir dir("edt_agent_policy_vcs");
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };
	BOOST_TEST((policy.effective_vcs().push_branches == std::vector<std::string>{ "agent/*" }));

	dir.write(".snk/agent_policy.json", R"({"vcs":{"push_branches":["bot/*"],"allow_hooks":true}})");
	BOOST_TEST((policy.effective_vcs().push_branches == std::vector<std::string>{ "bot/*" }));
	BOOST_TEST(policy.effective_vcs().permissions.allow_hooks);

	// A personal file that tries to turn a flag on is broken: everything closes.
	dir.write("personal.json", R"({"vcs":{"allow_filters":true}})");
	const edt::vcs_policy closed = policy.effective_vcs();
	BOOST_TEST(!policy.personal_error().empty());
	BOOST_TEST(closed.push_branches.empty());
	BOOST_TEST(!closed.permissions.allow_hooks);

	dir.write("personal.json", R"({"version":1,"vcs":{"allow_hooks":false}})");
	const edt::vcs_policy off = policy.effective_vcs();
	BOOST_TEST(policy.personal_error().empty());
	BOOST_TEST((off.push_branches == std::vector<std::string>{ "bot/*" }));
	BOOST_TEST(!off.permissions.allow_hooks);
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
	const fs::file_status probed = fs::status(locked / "agent_policy.json", ec);
	if (!ec && probed.type() != fs::file_type::none) {
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

namespace {

// A code.write as #451 has it: code_edit, but a CMake file is cmake_presets. `classify` stands for its category_of.
struct per_call_fixture : gate_fixture
{
	int asked_category = 0;
	std::function<agent_category(std::string_view)> classify = [](std::string_view path) {
		return path.ends_with("CMakeLists.txt") ? agent_category::cmake_presets : agent_category::code_edit;
	};

	per_call_fixture()
	{
		cmd::command_desc desc = command_of("code.write", agent_category::code_edit);
		desc.args_schema = boost::json::parse(R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"],"additionalProperties":false})");
		desc.category_of = [this](const cmd::call& request) {
			++asked_category;
			const auto& path = request.args.as_object().at("path").as_string();
			return classify(std::string_view(path.data(), path.size()));
		};
		desc.invoke = [this](const cmd::call&) {
			++runs;
			return cmd::result::success();
		};
		registrations.push_back(commands.add(std::move(desc)));
	}

	cmd::result write(boost::json::value path)
	{
		return commands.invoke("code.write", boost::json::object{ { "path", std::move(path) } }, cmd::origin::agent("s1"));
	}
};

}

BOOST_FIXTURE_TEST_CASE(ACallIsJudgedByTheStricterOfItsCommandsCategoryAndItsOwn, per_call_fixture)
{
	gate->set_can_ask(true);
	// Said here, not taken from the defaults (code_edit's default moves to ask, owner decision for #451).
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_edit":"allow"}})");

	BOOST_TEST(write("src/a.cpp").ok);
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::confirmation_required);
	BOOST_TEST(runs == 1);

	// A person's deny of code_edit covers the CMake writes of code.write too: the call's category never loosens.
	dir.write(".snk/agent_policy.json", R"({"categories":{"cmake_presets":"allow","code_edit":"deny"}})");
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::denied);
	BOOST_TEST(write("src/a.cpp").code == cmd::codes::denied);

	// Another size than the file before: the policy notices a change by modification time and size.
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_edit":"allow", "cmake_presets":"deny"}})");
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::denied);
	BOOST_TEST(write("src/a.cpp").ok);
	BOOST_TEST(runs == 2);
}

BOOST_FIXTURE_TEST_CASE(ACommandsOwnEntry_DoesNotSpeakForTheCategoryOfItsCall, per_call_fixture)
{
	gate->set_can_ask(true);
	dir.write(".snk/agent_policy.json", R"({"commands":{"code.write":"allow"}})");
	BOOST_TEST(write("src/a.cpp").ok);
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::confirmation_required);

	dir.write(".snk/agent_policy.json", R"({"commands":{"code.write":"allow"},"categories":{"cmake_presets":"deny"}})");
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::denied);
	BOOST_TEST(runs == 1);
}

// The --agent-policy file answers a call by the call's category: code_edit does not answer a CMake write (cmake_presets).
BOOST_FIXTURE_TEST_CASE(TheAgentPolicyFile_AnswersACallByItsOwnCategory, per_call_fixture)
{
	fs::create_directories(dir.path() / "project" / ".git");
	policy.set_headless(true);
	dir.write("answers.json", R"({"categories":{"code_edit":"allow"}})");
	policy.set_preanswers_file(dir.path() / "answers.json", dir.path() / "project");
	BOOST_TEST(write("src/a.cpp").ok);
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::denied);

	dir.write("answers2.json", R"({"categories":{"code_edit":"allow","cmake_presets":"allow"}})");
	policy.set_preanswers_file(dir.path() / "answers2.json", dir.path() / "project");
	BOOST_TEST(write("CMakeLists.txt").ok);
	BOOST_TEST(runs == 2);
}

BOOST_FIXTURE_TEST_CASE(ArgumentsThatBreakTheSchema_NeverReachCategoryOf, per_call_fixture)
{
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_edit":"allow"}})");
	BOOST_TEST(write(5).code == cmd::codes::invalid_args);
	BOOST_TEST(asked_category == 0);

	// Refused by the command's own category: the call learns nothing about its arguments.
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_edit":"deny"}})");
	BOOST_TEST(write(5).code == cmd::codes::denied);
	BOOST_TEST(asked_category == 0);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(ACallCategoryOfUnsetOrNotForAgents_IsDenied, per_call_fixture)
{
	classify = [](std::string_view) { return agent_category::unset; };
	BOOST_TEST(write("src/a.cpp").code == cmd::codes::denied);
	classify = [](std::string_view) { return agent_category::not_for_agents; };
	BOOST_TEST(write("src/a.cpp").code == cmd::codes::denied);
	BOOST_TEST(runs == 0);
}

BOOST_FIXTURE_TEST_CASE(ACategoryOfThatThrows_IsARefusal_AndLeavesNoTicket, per_call_fixture)
{
	gate->set_can_ask(true);
	classify = [](std::string_view) -> agent_category { throw std::runtime_error("broken"); };
	BOOST_TEST(write("CMakeLists.txt").code == cmd::codes::denied);
	BOOST_TEST(commands.pending_confirmations() == 0u);
	BOOST_TEST(runs == 0);
}

BOOST_AUTO_TEST_CASE(CategoryOfCall_WithoutAHook_IsTheCommandsCategory)
{
	cmd::command_desc plain = command_of("scene.move", agent_category::scene_edit);
	cmd::call request;
	request.args = boost::json::object{};
	BOOST_TEST((cmd::category_of_call(plain, request) == agent_category::scene_edit));
}

BOOST_AUTO_TEST_CASE(ALiveCommand_IsDeniedWhileLiveModeIsOff_WhateverCategoryItsCallHas)
{
	edt::policy_layer allow_all;
	allow_all.categories[agent_category::code_edit] = policy_mode::allow;
	allow_all.categories[agent_category::live] = policy_mode::allow;
	const cmd::command_desc live = command_of("live.rebuild_now", agent_category::live);
	const cmd::command_desc edit = command_of("code.write", agent_category::code_edit);
	BOOST_TEST((edt::effective_mode_of_call(live, agent_category::code_edit, allow_all, {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode_of_call(edit, agent_category::live, allow_all, {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode_of_call(live, agent_category::code_edit, allow_all, {}, {}, {}, true).mode == policy_mode::allow));
}

BOOST_AUTO_TEST_CASE(AnUnclassifiedCommand_StaysDenied_WhateverCategoryItsCallHas)
{
	edt::policy_layer allow_all;
	allow_all.categories[agent_category::code_edit] = policy_mode::allow;
	const cmd::command_desc unset = command_of("misc.thing", agent_category::unset);
	const cmd::command_desc not_for = command_of("edit.accept", agent_category::not_for_agents);
	BOOST_TEST((edt::effective_mode_of_call(unset, agent_category::code_edit, allow_all, {}).mode == policy_mode::deny));
	BOOST_TEST((edt::effective_mode_of_call(not_for, agent_category::code_edit, allow_all, {}).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_SUITE_END()

// The --agent-policy file (#452 PR-3): answers to `ask` given in advance, applied only in a headless editor.
BOOST_AUTO_TEST_SUITE(EdtAgentPolicyPreanswers)

namespace {

struct preanswers_fixture
{
	test_util::temp_dir dir{ "edt_agent_policy_preanswers" };
	edt::agent_policy policy{ dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json" };

	// A project that is its own git worktree; answers.json lies next to it, outside both.
	fs::path project_root = dir.path() / "project";

	preanswers_fixture()
	{
		fs::create_directories(project_root / ".git");
		policy.set_headless(true);
	}

	void answers(std::string_view text)
	{
		dir.write("answers.json", text);
		policy.set_preanswers_file(dir.path() / "answers.json", project_root);
	}

	policy_mode mode_of(const cmd::command_desc& command) { return policy.effective(command).mode; }
};

}

BOOST_FIXTURE_TEST_CASE(Headless_TheFileTurnsAskIntoAllow, preanswers_fixture)
{
	answers(R"({"categories":{"code_files":"allow"},"commands":{"code.write":"allow"}})");
	const auto create = command_of("code.create", agent_category::code_files);
	const auto write = command_of("code.write", agent_category::code_edit);

	BOOST_TEST(policy.preanswers_error().empty());
	BOOST_TEST((mode_of(create) == policy_mode::allow));
	BOOST_TEST((policy.effective(create).source == edt::policy_source::preanswers));
	BOOST_TEST((mode_of(write) == policy_mode::allow));
	// What the file does not answer still asks (and is a deny at the gate without a person).
	BOOST_TEST((mode_of(command_of("code.edit_other", agent_category::code_edit)) == policy_mode::ask));
}

BOOST_FIXTURE_TEST_CASE(WithAWindow_TheSameFileDoesNothing, preanswers_fixture)
{
	answers(R"({"categories":{"code_files":"allow","scene_edit":"deny"}})");
	policy.set_headless(false);

	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::ask));
	BOOST_TEST((mode_of(command_of("scene.move", agent_category::scene_edit)) == policy_mode::allow));

	policy.set_headless(true);
	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::allow));
}

BOOST_FIXTURE_TEST_CASE(TheProjectsDeny_Stays, preanswers_fixture)
{
	dir.write(".snk/agent_policy.json", R"({"categories":{"code_files":"deny"},"commands":{"scene.move":"deny"}})");
	answers(R"({"categories":{"code_files":"allow"},"commands":{"scene.move":"allow"}})");

	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("scene.move", agent_category::scene_edit)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(ThePersonalDeny_Stays, preanswers_fixture)
{
	dir.write("personal.json", R"({"categories":{"code_files":"deny"}})");
	answers(R"({"categories":{"code_files":"allow"}})");

	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(AnAllowForBuild_WhichIsDeniedByDefault_GivesNothing, preanswers_fixture)
{
	answers(R"({"categories":{"build":"allow"},"commands":{"build.game_module":"allow"}})");

	BOOST_TEST((mode_of(command_of("build.game_module", agent_category::build, cmd::danger::deny_by_default)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("build.other", agent_category::build)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(NeverOpensWhatNoFileCanOpen, preanswers_fixture)
{
	answers(R"({"commands":{"misc.thing":"allow","edit.accept":"allow","live.reload":"allow"}})");

	BOOST_TEST((mode_of(command_of("misc.thing", agent_category::unset)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("edit.accept", agent_category::not_for_agents)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("live.reload", agent_category::live)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(ACategoryAllow_DoesNotLiftARiskFloor_TheCommandsOwnEntryDoes, preanswers_fixture)
{
	answers(R"({"categories":{"read":"allow"}})");
	const auto risky = command_of("viewport.peek", agent_category::read, cmd::danger::confirm);
	BOOST_TEST((mode_of(risky) == policy_mode::ask));

	answers(R"({"commands":{"viewport.peek":"allow"}})");
	BOOST_TEST((mode_of(risky) == policy_mode::allow));
}

BOOST_FIXTURE_TEST_CASE(Tightening_AllowToDeny_WorksWithoutAPerson, preanswers_fixture)
{
	answers(R"({"categories":{"scene_edit":"deny"},"commands":{"scene.query":"ask"}})");

	BOOST_TEST((mode_of(command_of("scene.move", agent_category::scene_edit)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::ask));
	// A category "allow" never tightens a command the layers already allow, risk or not.
	answers(R"({"categories":{"read":"allow"}})");
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::allow));
}

BOOST_FIXTURE_TEST_CASE(AnAllowFlagInAVcsSection_IsAFileError_AndEverythingIsDenied, preanswers_fixture)
{
	answers(R"({"categories":{"code_files":"allow"},"vcs":{"allow_hooks":true}})");

	BOOST_TEST(!policy.preanswers_error().empty());
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::deny));
	BOOST_TEST((policy.effective(command_of("scene.query", agent_category::read)).source == edt::policy_source::preanswers));
}

BOOST_AUTO_TEST_CASE(AnyVcsSection_IsRefusedInTheFile_TheLayersStillTakeIt)
{
	for (const char* text : { R"({"vcs":{"allow_filters":false}})", R"({"vcs":{"push_branches":["agent/*"]}})", R"({"vcs":{}})" }) {
		BOOST_TEST_CONTEXT(text) {
			BOOST_TEST(!edt::parse_policy_layer(text, edt::policy_layer_kind::preanswers).error.empty());
			BOOST_TEST(edt::parse_policy_layer(text, edt::policy_layer_kind::project).error.empty());
		}
	}
}

BOOST_FIXTURE_TEST_CASE(AMissingFile_DeniesEverything_WithAnError, preanswers_fixture)
{
	std::vector<std::string> errors;
	const auto sink = engine::add_log_sink([&errors](std::string_view category, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::error && category == "editor/agent_policy") errors.emplace_back(message);
	});
	policy.set_preanswers_file(dir.path() / "no_such_answers.json", project_root);

	BOOST_TEST(errors.size() == 1u);
	BOOST_TEST(!policy.preanswers_error().empty());
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("scene.move", agent_category::scene_edit)) == policy_mode::deny));

	// Read once: a file that appears later changes nothing until a restart.
	dir.write("no_such_answers.json", "{}");
	BOOST_TEST((mode_of(command_of("scene.move", agent_category::scene_edit)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(AChangeWhileRunning_IsIgnored_WithOneWarning, preanswers_fixture)
{
	std::vector<std::string> warnings;
	const auto sink = engine::add_log_sink([&warnings](std::string_view category, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn && category == "editor/agent_policy") warnings.emplace_back(message);
	});
	answers(R"({"categories":{"code_files":"allow"}})");
	const auto create = command_of("code.create", agent_category::code_files);
	BOOST_TEST((mode_of(create) == policy_mode::allow));

	// Another size than before: the change is seen by modification time and size.
	dir.write("answers.json", R"({"categories":{"code_files":"deny","scene_edit":"deny"}})");
	BOOST_TEST((mode_of(create) == policy_mode::allow));
	dir.write("answers.json", R"({"categories":{"code_files":"ask"}})");
	BOOST_TEST((mode_of(create) == policy_mode::allow));
	BOOST_TEST((mode_of(command_of("scene.move", agent_category::scene_edit)) == policy_mode::allow));

	BOOST_TEST(std::ranges::count_if(warnings, [](const std::string& w) { return w.find("changed while running") != std::string::npos; }) == 1);
}

BOOST_FIXTURE_TEST_CASE(AFileInsideTheProject_DeniesEverything, preanswers_fixture)
{
	dir.write("project/answers.json", R"({"categories":{"code_files":"allow"}})");
	policy.set_preanswers_file(project_root / "answers.json", project_root);

	BOOST_TEST(policy.preanswers_error().find("must be outside the project") != std::string::npos);
	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::deny));
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(AFileInTheGitWorktreeOfTheProject_ButOutsideItsFolder_DeniesEverything, preanswers_fixture)
{
	fs::create_directories(dir.path() / "repo" / ".git");
	fs::create_directories(dir.path() / "repo" / "game");
	dir.write("repo/answers.json", R"({"categories":{"code_files":"allow"}})");
	policy.set_preanswers_file(dir.path() / "repo" / "answers.json", dir.path() / "repo" / "game");

	BOOST_TEST(policy.preanswers_error().find("must be outside the project") != std::string::npos);
	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(AFileOutsideTheProjectsGitWorktree_Works, preanswers_fixture)
{
	fs::create_directories(dir.path() / "repo" / ".git");
	fs::create_directories(dir.path() / "repo" / "game");
	dir.write("outside.json", R"({"categories":{"code_files":"allow"}})");
	policy.set_preanswers_file(dir.path() / "outside.json", dir.path() / "repo" / "game");

	BOOST_TEST(policy.preanswers_error().empty());
	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::allow));
}

BOOST_FIXTURE_TEST_CASE(ABrokenFile_DeniesEverything, preanswers_fixture)
{
	answers("{ not json");

	BOOST_TEST(!policy.preanswers_error().empty());
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::deny));
}

BOOST_FIXTURE_TEST_CASE(NoFileNamed_ChangesNothing, preanswers_fixture)
{
	BOOST_TEST(!policy.has_preanswers());
	BOOST_TEST((mode_of(command_of("code.create", agent_category::code_files)) == policy_mode::ask));
	BOOST_TEST((mode_of(command_of("scene.query", agent_category::read)) == policy_mode::allow));
}

BOOST_FIXTURE_TEST_CASE(ThroughTheGate_AnAnsweredAskRuns_OnlyHeadless, preanswers_fixture)
{
	answers(R"({"categories":{"code_files":"allow"}})");
	auto gate = std::make_shared<edt::agent_policy_gate>(policy);
	cmd::command_registry commands;
	int runs = 0;
	cmd::command_desc desc = command_of("code.create", agent_category::code_files);
	desc.invoke = [&runs](const cmd::call&) {
		++runs;
		return cmd::result::success();
	};
	const cmd::registration registration = commands.add(std::move(desc));
	commands.set_policy_gate(gate);

	BOOST_TEST(commands.invoke("code.create", boost::json::object{}, cmd::origin::agent("s1")).ok);
	// A window where nobody can be asked (a font without Cyrillic, before the first frame): the file does not answer.
	policy.set_headless(false);
	BOOST_TEST(commands.invoke("code.create", boost::json::object{}, cmd::origin::agent("s1")).code == cmd::codes::denied);
	gate->set_can_ask(true);
	BOOST_TEST(commands.invoke("code.create", boost::json::object{}, cmd::origin::agent("s1")).code == cmd::codes::confirmation_required);
	BOOST_TEST(runs == 1);
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(ApplyPreanswers_IsTheOnlyRule)
{
	const auto create = command_of("code.create", agent_category::code_files);
	edt::policy_layer file;
	file.categories[agent_category::code_files] = policy_mode::allow;
	const edt::effective_policy asked{ policy_mode::ask, edt::policy_source::default_table, "asks" };
	const edt::effective_policy denied{ policy_mode::deny, edt::policy_source::project, "no" };

	BOOST_TEST((edt::apply_preanswers(create, create.category, asked, file, {}).mode == policy_mode::allow));
	BOOST_TEST((edt::apply_preanswers(create, create.category, denied, file, {}).mode == policy_mode::deny));
	BOOST_TEST((edt::apply_preanswers(create, create.category, asked, file, "broken").mode == policy_mode::deny));
	// A call judged by another category is not answered by the command's own entry.
	edt::policy_layer by_command;
	by_command.commands["code.create"] = policy_mode::allow;
	BOOST_TEST((edt::apply_preanswers(create, agent_category::cmake_presets, asked, by_command, {}).mode == policy_mode::ask));
}

BOOST_AUTO_TEST_SUITE_END()
