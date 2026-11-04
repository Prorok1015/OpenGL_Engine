#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include "edt_agent_policy.h"
#include "edt_agent_policy_model.h"
#include "edt_mcp_bridge_host.h"
#include <snk/command/cmd_registry.h>
#include <algorithm>
#include <chrono>
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
	// The clock of the approval's arming delay: the tests move it.
	std::chrono::steady_clock::time_point fake_now{ std::chrono::seconds(100) };

	model_fixture()
	{
		policy = std::make_unique<edt::agent_policy>(project_file, personal_file,
			edt::approved_snapshot_location{ dir.path() / "approved" / "p.json", "/projects/model" });
		policy->set_approval_allowed(true);
		policy->set_command_source([this] { return std::vector<const cmd::command_desc*>{ &create, &write, &query, &hidden }; });
		model = std::make_unique<edt::agent_policy_model>(*policy, [this] {
			return std::vector<const cmd::command_desc*>{ &create, &write, &query, &hidden };
		}, [this] { return fake_now; });
		model->refresh();
	}

	void advance(std::chrono::milliseconds by) { fake_now += by; }
	// What a person does before Approve works: the window is used, and the copy stays on screen for a second.
	void arm()
	{
		model->note_review_opened();
		model->note_interaction();
		advance(edt::agent_policy_model::k_approve_arm_delay);
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
	BOOST_TEST(model->displayed_loosenings().size() == model->loosenings().size());
	BOOST_TEST((row_of(model->category_rows(edt::policy_scope::project), agent_category::code_files).effective.mode == policy_mode::ask));

	// The file changes after the person looked at it: the approval is refused, and the next look shows the new file.
	arm();
	write_project(R"({"categories":{"code_files":"allow","build":"ask","code_edit":"allow"}})");
	BOOST_TEST(model->approve().has_value());
	BOOST_TEST(model->pending());

	model->refresh();
	model->mark_displayed();
	BOOST_TEST(model->approve().has_value());
	arm();
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

BOOST_AUTO_TEST_CASE(AFileThatFlipsAfterTheBannerWasDrawn_IsNeverApprovedUnseen)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	arm();
	BOOST_REQUIRE(model->pending());
	const std::size_t shown = model->displayed_loosenings().size();

	// The file becomes another one (a checkout in between) and is read again later in the frame, as a stray refresh would.
	write_project(R"({"categories":{"code_files":"allow","build":"ask"}})");
	policy->refresh();
	BOOST_TEST(model->displayed_loosenings().size() == shown);
	BOOST_TEST(model->loosenings().size() > shown);

	const auto refused = model->approve();
	BOOST_REQUIRE(refused.has_value());
	BOOST_TEST(model->pending());
	BOOST_TEST(!policy->approved_project().categories.contains(agent_category::build));
	BOOST_TEST(!policy->approved_project().categories.contains(agent_category::code_files));
}

BOOST_AUTO_TEST_CASE(TheRowsOfTheWindow_ReadNoFile)
{
	write_project(R"({"categories":{"build":"allow"}})");
	model->refresh();
	model->mark_displayed();
	BOOST_REQUIRE(model->pending());

	write_project(R"({"categories":{"build":"allow","code_files":"allow","scene_edit":"deny"}})");
	const auto rows = model->category_rows(edt::policy_scope::project);
	BOOST_TEST((row_of(rows, agent_category::scene_edit).effective.mode == policy_mode::allow));
	BOOST_TEST(!policy->project().categories.contains(agent_category::scene_edit));
	BOOST_TEST(model->command_rows(edt::policy_scope::project, agent_category::code_files).size() == 1u);
	BOOST_TEST(!policy->project().categories.contains(agent_category::scene_edit));
}

BOOST_AUTO_TEST_CASE(Approve_NeedsTheCopyOnScreenForASecond_AndTheWindowUsed)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	BOOST_TEST(!model->approve_armed());
	model->note_review_opened();
	model->note_interaction();
	BOOST_TEST(!model->approve_armed());
	advance(std::chrono::milliseconds(999));
	BOOST_TEST(!model->approve_armed());
	BOOST_TEST(model->approve().has_value());
	BOOST_TEST(model->pending());
	advance(std::chrono::milliseconds(1));
	BOOST_TEST(model->approve_armed());
	// Drawn again every frame: the same copy does not restart the delay.
	model->mark_displayed();
	BOOST_TEST(model->approve_armed());
	BOOST_TEST(!model->approve().has_value());
	BOOST_TEST(!model->pending());
	BOOST_TEST(!model->approve_armed());
}

BOOST_AUTO_TEST_CASE(Approve_IsNotArmedWithoutTheWindowBeingUsed)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	model->note_review_opened();
	advance(std::chrono::seconds(30));
	BOOST_TEST(!model->approve_armed());
	BOOST_TEST(model->approve().has_value());
	BOOST_TEST(model->pending());
	model->note_interaction();
	BOOST_TEST(model->approve_armed());
}

BOOST_AUTO_TEST_CASE(Approve_IsDisarmedAgainWhenTheDisplayedLayerChanges)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	arm();
	BOOST_REQUIRE(model->approve_armed());

	write_project(R"({"categories":{"code_files":"allow","build":"ask"}})");
	model->refresh();
	model->mark_displayed();
	BOOST_TEST(!model->approve_armed());
	BOOST_TEST(model->approve().has_value());
	// Used and waited again, for the new copy.
	arm();
	BOOST_TEST(model->approve_armed());
	BOOST_TEST(!model->approve().has_value());
}

BOOST_AUTO_TEST_CASE(Approve_CountsFromTheReviewPopupOpening_NotFromTheBanner)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	// A long-visible banner, used by the mouse: still nothing to approve until the list itself was opened.
	model->note_interaction();
	advance(std::chrono::seconds(30));
	BOOST_TEST(!model->approve_armed());
	BOOST_TEST(model->approve().has_value());

	model->note_review_opened();
	model->note_interaction();
	BOOST_TEST(!model->approve_armed());
	advance(std::chrono::milliseconds(999));
	BOOST_TEST(!model->approve_armed());
	advance(std::chrono::milliseconds(1));
	BOOST_TEST(model->approve_armed());

	// Closed and opened again: from zero.
	model->note_review_closed();
	BOOST_TEST(!model->approve_armed());
	model->note_review_opened();
	model->note_interaction();
	BOOST_TEST(!model->approve_armed());
	advance(edt::agent_policy_model::k_approve_arm_delay);
	BOOST_TEST(model->approve_armed());
}

BOOST_AUTO_TEST_CASE(Approve_IsDisarmedWhenTheWindowWasHidden_AndStartsFromZeroWhenItComesBack)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	arm();
	BOOST_REQUIRE(model->approve_armed());

	model->forget_displayed();
	advance(std::chrono::seconds(60));
	model->mark_displayed();
	BOOST_TEST(!model->approve_armed());
	model->note_interaction();
	BOOST_TEST(!model->approve_armed());
	BOOST_TEST(model->approve().has_value());
	arm();
	BOOST_TEST(model->approve_armed());
}

BOOST_AUTO_TEST_CASE(MoreLooseningsThanThePopupLists_CanNotBeApprovedThere)
{
	std::string commands;
	for (std::size_t i = 0; i < edt::agent_policy_model::k_review_list_cap + 1; ++i) {
		commands += (commands.empty() ? "" : ",") + std::string("\"extra.cmd") + std::to_string(i) + "\":\"allow\"";
	}
	write_project(R"({"commands":{)" + commands + "}}");
	model->refresh();
	model->mark_displayed();
	BOOST_REQUIRE(model->displayed_loosenings().size() > edt::agent_policy_model::k_review_list_cap);
	BOOST_TEST(model->too_many_to_review());
	arm();
	advance(std::chrono::seconds(30));
	BOOST_TEST(!model->approve_armed());
	const auto refused = model->approve();
	BOOST_REQUIRE(refused.has_value());
	BOOST_TEST(refused->find("Too many changes") != std::string::npos);
	BOOST_TEST(model->pending());
}

BOOST_AUTO_TEST_CASE(Approve_RecordsOnlyWhatWasShown_NotACommandEntryThatLoosensNothing)
{
	// scene.query is allowed by default: its "allow" entry is no loosening, so the person was not shown it.
	write_project(R"({"categories":{"code_files":"allow"},"commands":{"scene.query":"allow","code.write":"allow"}})");
	model->refresh();
	model->mark_displayed();
	BOOST_REQUIRE(model->pending());
	arm();
	BOOST_TEST(!model->approve().has_value());
	const edt::policy_layer& approved = policy->approved_project();
	BOOST_TEST((approved.categories.at(agent_category::code_files) == policy_mode::allow));
	BOOST_TEST((approved.commands.at("code.write") == policy_mode::allow));
	BOOST_TEST(!approved.commands.contains("scene.query"));
}

BOOST_AUTO_TEST_CASE(NothingToApprove_ForgetsTheCopy)
{
	write_project(R"({"categories":{"code_files":"allow"}})");
	model->refresh();
	model->mark_displayed();
	arm();
	model->forget_displayed();
	BOOST_TEST(!model->approve_armed());
	BOOST_TEST(model->approve().has_value());
	BOOST_TEST(model->displayed_loosenings().empty());
}

BOOST_AUTO_TEST_CASE(Apply_SavesOnlyTheCommandEntriesThePersonChanged)
{
	write_project(R"({"commands":{"scene.query":"ask"}})");
	model->refresh();
	BOOST_TEST(!model->pending());

	BOOST_TEST(model->set_category(edt::policy_scope::project, agent_category::build, policy_mode::ask));
	BOOST_TEST(model->set_command(edt::policy_scope::project, "code.write", policy_mode::allow));
	BOOST_TEST(!model->apply().has_value());

	const edt::policy_layer& approved = policy->approved_project();
	BOOST_TEST((approved.categories.at(agent_category::build) == policy_mode::ask));
	BOOST_TEST((approved.commands.at("code.write") == policy_mode::allow));
	BOOST_TEST(!approved.commands.contains("scene.query"));
	BOOST_TEST(read_file(project_file).find("scene.query") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Apply_IsRefusedWhileTheFileMovedUnderTheEdit)
{
	write_project(R"({"categories":{"code_edit":"deny"}})");
	model->refresh();
	BOOST_TEST(model->set_category(edt::policy_scope::project, agent_category::build, policy_mode::ask));
	write_project(R"({"categories":{"code_edit":"deny","scene_edit":"ask"}})");
	model->refresh();
	BOOST_REQUIRE(model->base_changed(edt::policy_scope::project));

	BOOST_TEST(model->apply().has_value());
	BOOST_TEST(read_file(project_file).find("build") == std::string::npos);
	BOOST_TEST(model->dirty());
}

BOOST_AUTO_TEST_CASE(AGitListEntry_WithACommaOrControlCharacters_IsRefusedByTheModel)
{
	using edt::policy_scope;
	using edt::vcs_list_key;
	BOOST_TEST(!model->set_vcs_list(policy_scope::project, vcs_list_key::push_branches, std::vector<std::string>{ "a,b" }));
	BOOST_TEST(!model->set_vcs_list(policy_scope::project, vcs_list_key::remotes, std::vector<std::string>{ "o\x07k" }));
	BOOST_TEST(!model->set_vcs_list(policy_scope::project, vcs_list_key::protected_branches, std::vector<std::string>{ "x\xe2\x80\xae" }));
	BOOST_TEST(!model->set_vcs_list(policy_scope::project, vcs_list_key::push_branches, std::vector<std::string>{ std::string(257, 'a') }));
	BOOST_TEST(!model->dirty());
	BOOST_TEST(model->set_vcs_list(policy_scope::project, vcs_list_key::push_branches, std::vector<std::string>{ "agent/*", "bot/*" }));
	BOOST_TEST(model->dirty());
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
