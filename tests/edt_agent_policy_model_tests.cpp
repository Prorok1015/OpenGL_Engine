#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_agent_policy.h"
#include "edt_agent_policy_model.h"
#include "edt_mcp_bridge_host.h"
#include <snk/command/cmd_registry.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

// The policy part of the agents window (#449) without ImGui: what a person edits, what is written, the approval of a project
// file that loosens something, and that no command can reach any of it.
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

std::string read_file(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct model_fixture
{
	test_util::temp_dir dir{ "edt_agent_policy_model" };
	fs::path project_file = dir.path() / ".snk" / "agent_policy.json";
	fs::path personal_file = dir.path() / "personal.json";
	cmd::command_desc create = command_of("code.create", agent_category::code_files);
	cmd::command_desc write = command_of("code.write", agent_category::code_edit);
	cmd::command_desc query = command_of("scene.query", agent_category::read);
	cmd::command_desc hidden = command_of("dialog.answer", agent_category::not_for_agents);
	std::unique_ptr<edt::agent_policy> policy;
	std::unique_ptr<edt::agent_policy_model> model;

	model_fixture()
	{
		policy = std::make_unique<edt::agent_policy>(project_file, personal_file,
			edt::approved_snapshot_location{ dir.path() / "approved" / "p.json", "/projects/model" });
		policy->set_approval_allowed(true);
		policy->set_command_source([this] { return std::vector<const cmd::command_desc*>{ &create, &write, &query, &hidden }; });
		model = std::make_unique<edt::agent_policy_model>(*policy, [this] {
			return std::vector<const cmd::command_desc*>{ &create, &write, &query, &hidden };
		});
		model->refresh();
	}

	void write_project(const std::string& text) { dir.write(".snk/agent_policy.json", text); }

	const edt::policy_category_row& row_of(const std::vector<edt::policy_category_row>& rows, agent_category category)
	{
		const auto found = std::ranges::find(rows, category, &edt::policy_category_row::category);
		BOOST_REQUIRE(found != rows.end());
		return *found;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAgentPolicyModel, model_fixture)

BOOST_AUTO_TEST_CASE(EveryCategoryAFileCanSet_IsListedWithItsDefaultAndWhereItComesFrom)
{
	const auto rows = model->category_rows(edt::policy_scope::project);
	BOOST_TEST(rows.size() == edt::settable_categories().size());
	const auto& files = row_of(rows, agent_category::code_files);
	BOOST_TEST(!files.entry.has_value());
	BOOST_TEST((files.effective.mode == policy_mode::ask));
	BOOST_TEST((files.effective.source == edt::policy_source::default_table));
	BOOST_TEST((row_of(rows, agent_category::build).effective.mode == policy_mode::deny));
	BOOST_TEST((row_of(rows, agent_category::vcs_push).effective.mode == policy_mode::allow));
}

BOOST_AUTO_TEST_CASE(AnEditOfTheProjectLayer_IsWrittenByApply_AndNotBefore)
{
	BOOST_TEST(model->set_category(edt::policy_scope::project, agent_category::code_edit, policy_mode::deny));
	BOOST_TEST(model->dirty());
	BOOST_TEST(!fs::exists(project_file));
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_edit).entry == policy_mode::deny));
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_edit).effective.mode == policy_mode::ask));

	BOOST_TEST(!model->apply().has_value());
	BOOST_TEST(!model->dirty());
	BOOST_TEST(fs::exists(project_file));
	model->refresh();
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_edit).effective.mode == policy_mode::deny));
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_edit).effective.source == edt::policy_source::project));
}

BOOST_AUTO_TEST_CASE(RevertDropsTheEdits)
{
	model->set_category(edt::policy_scope::project, agent_category::build, policy_mode::ask);
	BOOST_TEST(model->dirty());
	model->revert();
	BOOST_TEST(!model->dirty());
	BOOST_TEST(!row_of(model->category_rows(edt::policy_scope::project), agent_category::build).entry.has_value());
}

BOOST_AUTO_TEST_CASE(ThePersonalLayer_CannotLoosenWhatTheProjectGives)
{
	// code_files asks by default: allow would loosen it, ask and deny do not.
	BOOST_TEST(!model->set_category(edt::policy_scope::personal, agent_category::code_files, policy_mode::allow));
	BOOST_TEST(!model->dirty());
	BOOST_TEST(model->set_category(edt::policy_scope::personal, agent_category::code_files, policy_mode::deny));
	BOOST_TEST(!model->apply().has_value());
	model->refresh();
	const auto rows = model->category_rows(edt::policy_scope::personal);
	BOOST_TEST((row_of(rows, agent_category::code_files).loosest == policy_mode::ask));
	BOOST_TEST((row_of(rows, agent_category::code_files).effective.mode == policy_mode::deny));
	BOOST_TEST((row_of(rows, agent_category::code_files).effective.source == edt::policy_source::personal));

	// A command is held to the same floor.
	BOOST_TEST(!model->set_command(edt::policy_scope::personal, "code.create", policy_mode::allow));
	BOOST_TEST(model->set_command(edt::policy_scope::personal, "code.create", policy_mode::deny));
}

BOOST_AUTO_TEST_CASE(ThePersonalLayer_MaySwitchAGitFlagOffNeverOn)
{
	BOOST_TEST(!model->set_vcs_flag(edt::policy_scope::personal, edt::vcs_flag_key::allow_hooks, true));
	BOOST_TEST(model->set_vcs_flag(edt::policy_scope::personal, edt::vcs_flag_key::allow_hooks, false));
	BOOST_TEST(!model->apply().has_value());
	BOOST_TEST(read_file(personal_file).find("allow_hooks") != std::string::npos);
	BOOST_TEST(edt::parse_policy_layer(read_file(personal_file), edt::policy_layer_kind::personal).error.empty());
}

BOOST_AUTO_TEST_CASE(TheQuickSwitch_IsTheCodeFilesCategory)
{
	BOOST_TEST(model->confirms_file_changes());
	model->set_confirms_file_changes(false);
	BOOST_TEST(!model->confirms_file_changes());
	BOOST_TEST(!model->apply().has_value());
	BOOST_TEST(read_file(project_file).find("\"code_files\": \"allow\"") != std::string::npos);

	model->refresh();
	BOOST_TEST(!model->confirms_file_changes());
	model->set_confirms_file_changes(true);
	BOOST_TEST(!model->apply().has_value());
	BOOST_TEST(read_file(project_file).find("code_files") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(TheGitLists_AreEditedAndAnEmptySectionIsNoSection)
{
	BOOST_TEST(model->set_vcs_list(edt::policy_scope::project, edt::vcs_list_key::push_branches, std::vector<std::string>{ "agent/*", "bot/*" }));
	BOOST_REQUIRE(model->layer(edt::policy_scope::project).vcs.has_value());
	BOOST_TEST((model->layer(edt::policy_scope::project).vcs->push_branches == std::vector<std::string>{ "agent/*", "bot/*" }));
	BOOST_TEST(model->set_vcs_list(edt::policy_scope::project, edt::vcs_list_key::push_branches, std::nullopt));
	BOOST_TEST(!model->layer(edt::policy_scope::project).vcs.has_value());
	BOOST_TEST(!model->dirty());

	// Explicitly no push at all is not the same as no entry.
	BOOST_TEST(model->set_vcs_list(edt::policy_scope::project, edt::vcs_list_key::push_branches, std::vector<std::string>{}));
	BOOST_TEST(model->dirty());
}

BOOST_AUTO_TEST_CASE(ALayerThatCannotBeWritten_IsRefusedAndNothingIsDropped)
{
	BOOST_TEST(model->set_command(edt::policy_scope::project, "bad id with spaces", policy_mode::allow));
	const auto error = model->apply();
	BOOST_REQUIRE(error.has_value());
	BOOST_TEST(!fs::exists(project_file));
	BOOST_TEST(model->dirty());
}

BOOST_AUTO_TEST_CASE(CommandRows_ListTheCommandsOfACategoryAnAgentCouldCall_ByName)
{
	const auto rows = model->command_rows(edt::policy_scope::project, agent_category::code_files);
	BOOST_REQUIRE_EQUAL(rows.size(), 1u);
	BOOST_TEST(rows[0].id == "code.create");
	BOOST_TEST((rows[0].effective.mode == policy_mode::ask));
	// A command no agent can call is not offered, whatever its category says.
	BOOST_TEST(model->command_rows(edt::policy_scope::project, agent_category::not_for_agents).empty());
}

BOOST_AUTO_TEST_CASE(AFileThatLoosensSomething_WaitsAndIsApprovedOnlyAsShown)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	BOOST_TEST(model->pending());
	BOOST_REQUIRE(!model->loosenings().empty());
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_files).effective.mode == policy_mode::ask));

	// The file changes after the person looked at it: the approval is refused, and the next look shows the new file.
	write_project(R"({"categories":{"code_files":"allow","build":"ask","code_edit":"allow"}})");
	BOOST_TEST(model->approve().has_value());
	BOOST_TEST(model->pending());

	model->refresh();
	model->mark_displayed();
	BOOST_TEST(!model->approve().has_value());
	BOOST_TEST(!model->pending());
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_files).effective.mode == policy_mode::allow));
}

BOOST_AUTO_TEST_CASE(NothingIsApprovedBeforeTheFileWasShown)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	policy->refresh();
	BOOST_TEST(model->approve().has_value());
	BOOST_TEST(policy->project_pending_approval());
}

BOOST_AUTO_TEST_CASE(EditsAreMadeOnTopOfTheApprovedLayer_SoThePendingLooseningsAreDropped)
{
	write_project(R"({"categories":{"code_files":"allow","build":"ask"}})");
	model->refresh();
	model->mark_displayed();
	BOOST_REQUIRE(model->pending());

	BOOST_TEST(model->set_category(edt::policy_scope::project, agent_category::scene_edit, policy_mode::ask));
	BOOST_TEST(model->apply_drops_pending());
	BOOST_TEST(!model->apply().has_value());
	BOOST_TEST(!model->pending());

	const std::string text = read_file(project_file);
	BOOST_TEST(text.find("scene_edit") != std::string::npos);
	BOOST_TEST(text.find("code_files") == std::string::npos);
	BOOST_TEST(text.find("build") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(AnEditWhoseFileChangedUnderIt_IsKeptAndFlagged_AnUneditedLayerFollowsTheFile)
{
	// No edit: the layer follows the file.
	write_project(R"({"categories":{"code_edit":"deny"}})");
	model->refresh();
	BOOST_TEST((model->layer(edt::policy_scope::project).categories.at(agent_category::code_edit) == policy_mode::deny));

	BOOST_TEST(model->set_category(edt::policy_scope::project, agent_category::build, policy_mode::ask));
	write_project(R"({"categories":{"code_edit":"deny","scene_edit":"ask"}})");
	model->refresh();
	BOOST_TEST(model->base_changed(edt::policy_scope::project));
	BOOST_TEST(model->dirty());
	BOOST_TEST(model->layer(edt::policy_scope::project).categories.contains(agent_category::build));
	model->revert();
	model->refresh();
	BOOST_TEST(!model->base_changed(edt::policy_scope::project));
	BOOST_TEST(model->layer(edt::policy_scope::project).categories.contains(agent_category::scene_edit));
}

BOOST_AUTO_TEST_CASE(NoCommandOfTheWindowIsOfferedToAgents)
{
	// The window's own action is a UI affair: not for agents, so never a tool, never callable, whatever any file says.
	cmd::command_desc open = command_of("agent.settings.open", agent_category::not_for_agents);
	open.expose_as_tool = false;
	write_project(R"({"commands":{"agent.settings.open":"allow"},"categories":{"not_for_agents":"allow"}})");
	policy->refresh();
	BOOST_TEST(!policy->project_error().empty());
	BOOST_TEST(!edt::agent_may_use(open, *policy));
	BOOST_TEST((policy->effective(open).mode == policy_mode::deny));

	write_project(R"({"commands":{"agent.settings.open":"allow"}})");
	policy->refresh();
	BOOST_TEST(policy->project_error().empty());
	BOOST_TEST(!edt::agent_may_use(open, *policy));
	BOOST_TEST((policy->effective(open).mode == policy_mode::deny));
}

BOOST_AUTO_TEST_SUITE_END()
