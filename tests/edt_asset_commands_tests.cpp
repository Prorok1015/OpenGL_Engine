#include <boost/test/unit_test.hpp>
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_policy.h"
#include "edt_asset_commands.h"
#include "edt_asset_database.h"
#include "edt_asset_history.h"
#include "edt_asset_ops.h"
#include <snk/command/cmd_registry.h>
#include <snk/common/fs_paths.h>
#include <snk/editor/edt_document.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_asset_meta.h>
#include <snk/resource/res_pretty_json.h>
#include <boost/json.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// asset.rename / asset.move / asset.delete (#775, EPIC-57 section 6.4, PR 2) over a real project folder, the history of the
// operations and its rebuild after a restart.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

constexpr std::string_view k_sky_guid = "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f";
constexpr std::string_view k_other_guid = "5b1d2e3f-4a5b-4c6d-8e7f-0a1b2c3d4e5f";
constexpr std::string_view k_lonely_guid = "6c2e3f40-5b6c-4d7e-8f80-1b2c3d4e5f60";
constexpr std::string_view k_rock_guid = "7d3f4051-6c7d-4e8f-9091-2c3d4e5f6071";
constexpr std::string_view k_rock_bin_guid = "8e405162-7d8e-4f90-a1a2-3d4e5f607182";

std::string meta_text(std::string_view guid)
{
	res::asset_meta meta;
	meta.guid = *res::asset_id_from_text(guid);
	meta.importer = "test";
	return res::write_asset_meta(meta);
}

std::string canonical(std::string_view text)
{
	const std::vector<std::byte> bytes = res::pretty_json_bytes(json::parse(text));
	return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

const char* const k_level = R"({
	"__type": "level_desc",
	"worlds": [ { "__type": "world_desc", "name": "w", "children": [ { "__id": "00000000000000a1",
		"components": {
			"sky": "res://sky.desc",
			"mesh": { "__type": "mesh_node_desc", "material": { "guid": "3f2a9c1e-7b4d-4e8a-9c0f-1a2b3c4d5e6f", "path": "res://old_sky.desc" } },
			"inst": { "__type": "prefab_desc", "__parent": "res://sky.desc" },
			"other": { "__type": "mesh_node_desc", "material": "res://other.desc" }
		} } ] } ]
})";

std::string text_of(const json::value& value)
{
	return std::string(value.as_string());
}

bool has_string(const json::array& items, std::string_view key, std::string_view wanted)
{
	for (const json::value& item : items) {
		const json::value* found = item.as_object().if_contains(json::string_view(key.data(), key.size()));
		if (found && found->is_string() && std::string_view(found->as_string()) == wanted) return true;
	}
	return false;
}

struct commands_fixture
{
	commands_fixture() : layer("asset_cmd"), engine("asset_cmd_engine"), journal("asset_cmd_journal"), policy_dir("asset_cmd_policy"), user_data("asset_cmd_user")
	{
		write("sky.desc", canonical(R"({ "__type": "material_desc" })"));
		write("sky.desc.resource", meta_text(k_sky_guid));
		write("other.desc", canonical(R"({ "__type": "material_desc" })"));
		write("other.desc.resource", meta_text(k_other_guid));
		write("lonely.desc", canonical(R"({ "__type": "material_desc" })"));
		write("lonely.desc.resource", meta_text(k_lonely_guid));
		write("env/.keep", "x");
		write("levels/main.desc", canonical(k_level));
		write("materials/child.desc", canonical(R"({ "__type": "material_desc", "__parent": "res://sky.desc" })"));

		edt::asset_command_sources sources;
		sources.operations = [this]() -> const edt::asset_ops* { return project_open ? &operations : nullptr; };
		sources.write_blocker = [this] { return blocked; };
		sources.asset_index_wait = [this] { return waiting; };
		sources.done = [this](const edt::operation_report& report) { history.record(report.journal); };
		sources.events = &events;
		registrations = edt::register_asset_commands(commands, std::move(sources));
		seen = events.agent_action.subscribe([this](const edt::agent_action_info& info) { actions.push_back(info); });
	}

	void write(std::string_view relative, std::string_view text)
	{
		const fs::path path = layer.write(relative, text);
		fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours(1));
	}

	edt::asset_database& open()
	{
		db = std::make_unique<edt::asset_database>(jobs, &events, std::vector<fs::path>{ engine.path(), layer.path() });
		db->rebuild();
		test_util::pump(tasks);
		return *db;
	}

	// The hashing of what an operation changed is done before the next one is planned: until then the database says it is still reading.
	void settle() { test_util::pump(tasks); }

	edt::asset_ops make_operations()
	{
		edt::asset_ops_ports ports;
		ports.assets = [this]() -> const edt::asset_database* { return db.get(); };
		ports.engine_base = engine.path();
		ports.journal_root = journal.path();
		ports.is_open_with_unsaved_changes = [this](const res::tag& tag) { return open_files.contains(std::string(tag.view())); };
		ports.hold_changes = []() -> std::shared_ptr<void> { return std::make_shared<int>(0); };
		ports.report_self_change = [this](std::span<const res::file_change> changes) {
			if (db) db->report_self_change(changes);
		};
		return edt::asset_ops(std::move(ports));
	}

	cmd::result call(const std::string& id, json::object args, cmd::origin from = cmd::origin::test())
	{
		return commands.invoke(id, json::value(std::move(args)), std::move(from));
	}

	static cmd::call agent_call(json::object args)
	{
		cmd::call out;
		out.args = std::move(args);
		out.from = cmd::origin::agent("s1");
		return out;
	}

	void ask_a_person()
	{
		gate = std::make_shared<edt::agent_policy_gate>(policy);
		gate->set_can_ask(true);
		commands.set_policy_gate(gate);
	}

	std::string file_text(std::string_view relative) const
	{
		std::ifstream in(layer.path() / std::string(relative), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	bool has(std::string_view relative) const { return fs::is_regular_file(layer.path() / std::string(relative)); }

	std::map<std::string, std::string> snapshot() const
	{
		std::map<std::string, std::string> out;
		for (const fs::directory_entry& entry : fs::recursive_directory_iterator(layer.path())) {
			if (!entry.is_regular_file()) continue;
			const std::string relative = entry.path().lexically_relative(layer.path()).generic_string();
			out[relative] = file_text(relative);
		}
		return out;
	}

	// The editor's own list of its journals: in the person's data, not in the project.
	fs::path list_file() const { return edt::asset_history_file({}, user_data.path(), journal.path()); }

	edt::asset_history make_history(std::uint64_t first_serial = 0)
	{
		return edt::asset_history{ journal.path(), list_file(), [n = first_serial]() mutable { return ++n; } };
	}

	std::vector<fs::path> roots() const { return { layer.path() }; }

	test_util::temp_dir layer;
	test_util::temp_dir engine;
	test_util::temp_dir journal;
	test_util::temp_dir policy_dir;
	test_util::temp_dir user_data;
	ds::task_system tasks{ test_util::manual_tasks() };
	edt::editor_events events;
	edt::job_service jobs{ tasks, &events };
	std::unique_ptr<edt::asset_database> db;
	std::set<std::string> open_files;
	edt::asset_ops operations{ make_operations() };
	edt::asset_history history{ journal.path(), list_file(), [this] { return db->next_history_serial(); } };
	edt::agent_policy policy{ policy_dir.path() / "no_project_policy.json", policy_dir.path() / "no_personal_policy.json" };
	std::shared_ptr<edt::agent_policy_gate> gate;
	cmd::command_registry commands;
	std::vector<edt::registration> registrations;
	std::vector<edt::agent_action_info> actions;
	edt::connection seen;
	std::optional<std::string> blocked;
	std::optional<std::string> waiting;
	bool project_open = true;
};

class fake_document final : public edt::document
{
public:
	fake_document(std::string name, bool dirty) : m_title(std::move(name)), m_source(res::tag::make(m_title)), m_dirty(dirty) {}

	std::string_view kind() const override { return "fake"; }
	const std::string& title() const override { return m_title; }
	const res::tag& source() const override { return m_source; }
	bool is_dirty() const override { return m_dirty; }
	std::uint64_t generation() const override { return 0; }

private:
	std::string m_title;
	res::tag m_source;
	bool m_dirty = false;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAssetCommandsTests, commands_fixture)

BOOST_AUTO_TEST_CASE(ARenameByTheTestOrigin_RunsAndAnswersWithAReportOfTheOperation)
{
	open();
	const cmd::result result = call("asset.rename", json::object{ { "path", "res://sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(!has("sky.desc"));
	BOOST_TEST(has("clouds.desc"));

	const json::object& report = result.value.as_object();
	BOOST_TEST(text_of(report.at("action")) == "rename");
	BOOST_TEST(!text_of(report.at("operation")).empty());
	BOOST_TEST((history.next_operation_id() == std::optional<std::string>(text_of(report.at("operation")))), "the id names the entry of the history");
	BOOST_REQUIRE(report.at("moved").as_array().size() == 2u);
	BOOST_TEST(text_of(report.at("moved").as_array()[0].at("from")) == "res://sky.desc");
	BOOST_TEST(text_of(report.at("moved").as_array()[0].at("to")) == "res://clouds.desc");
	BOOST_REQUIRE(report.at("rewritten").as_array().size() == 2u);
	BOOST_TEST(has_string(report.at("rewritten").as_array(), "file", "res://levels/main.desc"));
	BOOST_TEST(has_string(report.at("rewritten").as_array(), "file", "res://materials/child.desc"));
	for (const json::value& file : report.at("rewritten").as_array()) BOOST_TEST(!file.at("pointers").as_array().empty());
	BOOST_TEST(report.at("could_not_rewrite").as_array().empty());
	BOOST_TEST(report.at("warnings").as_array().empty());
	BOOST_TEST(report.at("broken_references").as_array().empty());

	BOOST_TEST(history.size() == 1u);
	BOOST_TEST((history.next_label() == std::optional<std::string>("Rename sky.desc")));
	BOOST_TEST(actions.empty(), "an agent_action is for an agent's change only");
}

BOOST_AUTO_TEST_CASE(AMoveAndADelete_AreEntriesOfTheHistoryToo_UndoneNewestFirst)
{
	open();
	const auto before = snapshot();
	const cmd::result moved = call("asset.move", json::object{ { "path", "sky.desc" }, { "to", "env" } });
	BOOST_REQUIRE_MESSAGE(moved.ok, moved.error);
	BOOST_TEST(has("env/sky.desc"));
	BOOST_TEST(text_of(moved.value.at("moved").as_array()[0].at("to")) == "res://env/sky.desc");
	settle();

	const cmd::result removed = call("asset.delete", json::object{ { "path", "lonely.desc" } });
	BOOST_REQUIRE_MESSAGE(removed.ok, removed.error);
	BOOST_TEST(!has("lonely.desc"));
	const std::string stashed = text_of(removed.value.at("moved").as_array()[0].at("to"));
	BOOST_TEST(stashed.starts_with("cache://asset_ops/"));
	BOOST_TEST(text_of(removed.value.at("action")) == "delete");
	BOOST_TEST(history.size() == 2u);
	BOOST_TEST((history.next_label() == std::optional<std::string>("Delete lonely.desc")));

	const edt::asset_fix_result first = history.undo_last(operations);
	BOOST_REQUIRE_MESSAGE(first.ok, first.error);
	BOOST_TEST(has("lonely.desc"));
	BOOST_TEST(has("env/sky.desc"));
	BOOST_TEST((history.next_label() == std::optional<std::string>("Move sky.desc")));
	const edt::asset_fix_result second = history.undo_last(operations);
	BOOST_REQUIRE_MESSAGE(second.ok, second.error);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);
	BOOST_TEST(!history.undo_last(operations).ok, "nothing is left to undo");
}

BOOST_AUTO_TEST_CASE(TheLabelOfAPairsOperation_NamesTheDesc_NeverTheBinaryFileOrAFileOfTheJournalFolder)
{
	write("meshes/rock.mesh.desc", canonical(R"({ "__type": "mesh_desc" })"));
	write("meshes/rock.mesh.desc.resource", meta_text(k_rock_guid));
	write("meshes/rock.mesh.bin", std::string("SNKB\x01\x00\x00\x00 geometry", 17));
	write("meshes/rock.mesh.bin.resource", meta_text(k_rock_bin_guid));
	open();

	const cmd::result renamed = call("asset.rename", json::object{ { "path", "meshes/rock.mesh.desc" }, { "to", "stone.mesh.desc" } });
	BOOST_REQUIRE_MESSAGE(renamed.ok, renamed.error);
	BOOST_TEST(has("meshes/stone.mesh.bin"));
	BOOST_TEST((history.next_label() == std::optional<std::string>("Rename rock.mesh.desc")));
	settle();

	const cmd::result removed = call("asset.delete", json::object{ { "path", "meshes/stone.mesh.desc" } });
	BOOST_REQUIRE_MESSAGE(removed.ok, removed.error);
	BOOST_TEST(!has("meshes/stone.mesh.bin"));
	BOOST_TEST((history.next_label() == std::optional<std::string>("Delete stone.mesh.desc")));

	edt::operation_journal journal;
	journal.operation = edt::asset_operation::move;
	const auto step = [](edt::journal_step_kind kind, std::string from) { return edt::journal_step{ kind, "/p", std::move(from), {}, {}, {}, {} }; };
	journal.steps = { step(edt::journal_step_kind::rewrite, "levels/main.desc"), step(edt::journal_step_kind::move, "meshes/a.mesh.bin.resource"),
		step(edt::journal_step_kind::move, "meshes/a.mesh.bin"), step(edt::journal_step_kind::move, "meshes/a.mesh.desc.resource"),
		step(edt::journal_step_kind::move, "meshes/a.mesh.desc") };
	BOOST_TEST(edt::asset_operation_label(journal) == "Move a.mesh.desc");
	journal.steps = { step(edt::journal_step_kind::move, "pics/a.png.resource"), step(edt::journal_step_kind::move, "pics/a.png") };
	BOOST_TEST(edt::asset_operation_label(journal) == "Move a.png");
	journal.steps = { step(edt::journal_step_kind::move, "a.mesh.bin") };
	BOOST_TEST(edt::asset_operation_label(journal) == "Move a.mesh.bin", "nothing else to name: the file that was moved");
}

BOOST_AUTO_TEST_CASE(ADeleteOfAFileThatIsReferredTo_IsRefusedWithTheirList_AndNothingChanges)
{
	open();
	const auto before = snapshot();
	const cmd::result result = call("asset.delete", json::object{ { "path", "sky.desc" } });
	BOOST_REQUIRE(!result.ok);
	BOOST_TEST(result.code == cmd::codes::needs_input);
	BOOST_TEST(result.error.find("has_referrers") != std::string::npos);
	BOOST_TEST(result.error.find("res://levels/main.desc") != std::string::npos);
	BOOST_TEST(text_of(result.value.at("code")) == "has_referrers");
	BOOST_TEST(!result.value.at("referrers").as_array().empty());
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);
}

BOOST_AUTO_TEST_CASE(AnAgentsRename_IsAskedAbout_AndRunsOnlyWithTheApprovedTicket)
{
	open();
	ask_a_person();
	const auto before = snapshot();
	cmd::call request = agent_call(json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	const cmd::result asked = commands.invoke("asset.rename", request);
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);

	const std::string ticket(asked.value.at("ticket").as_string());
	const auto pending = commands.confirmation_of(ticket);
	BOOST_REQUIRE(pending.has_value());
	BOOST_TEST(pending->detail.find("Rename the asset") != std::string::npos);
	BOOST_TEST(pending->detail.find("res://sky.desc") != std::string::npos);
	BOOST_TEST(pending->detail.find("clouds.desc") != std::string::npos);
	BOOST_TEST(pending->detail.find("res://levels/main.desc") != std::string::npos, "the files that are rewritten are named");

	BOOST_REQUIRE(commands.approve(ticket));
	request.confirmation = ticket;
	const cmd::result done = commands.invoke("asset.rename", request);
	BOOST_REQUIRE_MESSAGE(done.ok, done.error);
	BOOST_TEST(has("clouds.desc"));
	BOOST_REQUIRE(actions.size() == 1u);
	BOOST_TEST(actions[0].command == "asset.rename");
	BOOST_TEST(actions[0].target == "res://sky.desc -> res://clouds.desc");
	BOOST_TEST(actions[0].session == "s1");
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AnAgentsDeleteWithoutAnApprovedTicket_NeverRuns)
{
	open();
	ask_a_person();
	cmd::call request = agent_call(json::object{ { "path", "lonely.desc" } });
	const cmd::result asked = commands.invoke("asset.delete", request);
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	request.confirmation = std::string(asked.value.at("ticket").as_string());
	const cmd::result unapproved = commands.invoke("asset.delete", request);
	BOOST_TEST(unapproved.code == cmd::codes::denied);
	BOOST_TEST(has("lonely.desc"));

	gate->set_can_ask(false);
	cmd::call nobody = agent_call(json::object{ { "path", "lonely.desc" } });
	BOOST_TEST(commands.invoke("asset.delete", nobody).code == cmd::codes::denied, "nobody to ask: a denial");
	BOOST_TEST(has("lonely.desc"));
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AForcedDelete_IsAskedApart_WithTheReferencesThatWouldBreak)
{
	open();
	ask_a_person();
	cmd::call request = agent_call(json::object{ { "path", "sky.desc" }, { "force", true } });
	const cmd::result asked = commands.invoke("asset.delete", request);
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	const std::string ticket(asked.value.at("ticket").as_string());
	const auto pending = commands.confirmation_of(ticket);
	BOOST_REQUIRE(pending.has_value());
	BOOST_TEST(pending->detail.find("FORCE") != std::string::npos);
	BOOST_TEST(pending->detail.find("res://levels/main.desc") != std::string::npos);
	BOOST_TEST(pending->detail.find("res://materials/child.desc") != std::string::npos);
	BOOST_TEST(pending->detail.find("/worlds/0/children/0/components/sky") != std::string::npos, "each place is named by its file and JSON pointer");
	BOOST_TEST(has("sky.desc"));

	// The ticket of a delete without force is not one of a forced delete.
	cmd::call plain = agent_call(json::object{ { "path", "sky.desc" } });
	const cmd::result plain_asked = commands.invoke("asset.delete", plain);
	BOOST_REQUIRE(plain_asked.code == cmd::codes::confirmation_required);
	const std::string plain_ticket(plain_asked.value.at("ticket").as_string());
	BOOST_REQUIRE(commands.approve(plain_ticket));
	request.confirmation = plain_ticket;
	BOOST_TEST(commands.invoke("asset.delete", request).code == cmd::codes::denied);
	BOOST_TEST(has("sky.desc"));

	BOOST_REQUIRE(commands.approve(ticket));
	request.confirmation = ticket;
	const cmd::result done = commands.invoke("asset.delete", request);
	BOOST_REQUIRE_MESSAGE(done.ok, done.error);
	BOOST_TEST(!has("sky.desc"));
	BOOST_TEST(!done.value.at("broken_references").as_array().empty());
	BOOST_TEST(!done.value.at("warnings").as_array().empty());
	BOOST_TEST(history.size() == 1u);
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AnAgentsForce_ThatNoPersonConfirmed_IsRefusedWhateverThePolicySays)
{
	open();
	const auto before = snapshot();
	// No gate: the policy lets every call of an agent through, so nobody was asked.
	const cmd::result result = commands.invoke("asset.delete", agent_call(json::object{ { "path", "sky.desc" }, { "force", true } }));
	BOOST_TEST(result.code == cmd::codes::denied);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);

	// A person's own call is not asked, and is the person's to force.
	const cmd::result own = call("asset.delete", json::object{ { "path", "sky.desc" }, { "force", true } }, cmd::origin::ui());
	BOOST_TEST(own.ok);
	BOOST_TEST(!has("sky.desc"));
}

BOOST_AUTO_TEST_CASE(AnAgentsRename_RunsOnlyWhileThePlanIsTheOneThePersonApproved)
{
	open();
	ask_a_person();
	cmd::call request = agent_call(json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	const cmd::result asked = commands.invoke("asset.rename", request);
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	const std::string ticket(asked.value.at("ticket").as_string());
	BOOST_REQUIRE(commands.approve(ticket));

	// A file that would be rewritten is another file now: what the person was shown is not what would be done.
	std::string changed = file_text("levels/main.desc");
	const std::size_t at = changed.find("res://other.desc");
	BOOST_REQUIRE(at != std::string::npos);
	changed.replace(at, std::string_view("res://other.desc").size(), "res://lonely.desc");
	write("levels/main.desc", changed);
	const auto after_edit = snapshot();

	request.confirmation = ticket;
	const cmd::result stale = commands.invoke("asset.rename", request);
	BOOST_REQUIRE(!stale.ok);
	BOOST_TEST(stale.code == cmd::codes::stale);
	BOOST_TEST(text_of(stale.value.at("code")) == "plan_changed");
	BOOST_TEST((snapshot() == after_edit), "nothing was done");
	BOOST_TEST(history.size() == 0u);

	// Asked again, the question is about the plan as it is, and that one runs.
	cmd::call again = agent_call(json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	const cmd::result asked_again = commands.invoke("asset.rename", again);
	BOOST_REQUIRE(asked_again.code == cmd::codes::confirmation_required);
	const std::string second(asked_again.value.at("ticket").as_string());
	BOOST_REQUIRE(commands.approve(second));
	again.confirmation = second;
	const cmd::result done = commands.invoke("asset.rename", again);
	BOOST_REQUIRE_MESSAGE(done.ok, done.error);
	BOOST_TEST(has("clouds.desc"));
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AQuestionThatSaysItWillBeRefused_HasNothingToSpend_WhateverTheFilesBecome)
{
	open();
	ask_a_person();
	cmd::call request = agent_call(json::object{ { "path", "ghost.desc" } });
	const cmd::result asked = commands.invoke("asset.delete", request);
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	const std::string ticket(asked.value.at("ticket").as_string());
	const auto pending = commands.confirmation_of(ticket);
	BOOST_REQUIRE(pending.has_value());
	BOOST_TEST(pending->detail.find("This request will be refused") != std::string::npos);
	BOOST_REQUIRE(commands.approve(ticket));

	write("ghost.desc", canonical(R"({ "__type": "material_desc" })"));
	write("ghost.desc.resource", meta_text("a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d"));
	open();
	request.confirmation = ticket;
	const cmd::result result = commands.invoke("asset.delete", request);
	BOOST_TEST(!result.ok);
	BOOST_TEST(result.code == cmd::codes::denied);
	BOOST_TEST(has("ghost.desc"), "the approved 'refused' question did not delete what appeared since");
	BOOST_TEST(history.size() == 0u);

	// A delete of something that refers to others needs `force`: the question without it is a refusal, not an approvable plan.
	cmd::call referred = agent_call(json::object{ { "path", "sky.desc" } });
	const cmd::result referred_asked = commands.invoke("asset.delete", referred);
	BOOST_REQUIRE(referred_asked.code == cmd::codes::confirmation_required);
	const std::string referred_ticket(referred_asked.value.at("ticket").as_string());
	BOOST_REQUIRE(commands.approve(referred_ticket));
	referred.confirmation = referred_ticket;
	BOOST_TEST(commands.invoke("asset.delete", referred).code == cmd::codes::needs_input, "refused for its referrers, whatever was approved");
	BOOST_TEST(has("sky.desc"));
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AProjectPolicyThatAllowsRenameOrMove_IsNotAYes_TheyAreAskedAbout_AndDeleteStillNeedsAPerson)
{
	open();
	policy_dir.write("no_project_policy.json", R"({"commands":{"asset.delete":"allow","asset.rename":"allow","asset.move":"allow"}})");
	policy_dir.write("no_personal_policy.json", R"({"commands":{"asset.rename":"allow","asset.move":"allow"}})");
	policy.set_approval_allowed(true);
	policy.refresh();
	BOOST_REQUIRE(!policy.approve_project_layer(policy.project()).has_value());
	ask_a_person();
	const auto before = snapshot();

	const cmd::result deleted = commands.invoke("asset.delete", agent_call(json::object{ { "path", "lonely.desc" } }));
	BOOST_TEST(deleted.code == cmd::codes::denied);
	BOOST_TEST(deleted.error.find("confirmed") != std::string::npos);
	BOOST_TEST((snapshot() == before));

	cmd::call move = agent_call(json::object{ { "path", "lonely.desc" }, { "to", "env" } });
	const cmd::result move_asked = commands.invoke("asset.move", move);
	BOOST_REQUIRE(move_asked.code == cmd::codes::confirmation_required);
	BOOST_TEST((snapshot() == before));

	cmd::call request = agent_call(json::object{ { "path", "lonely.desc" }, { "to", "alone.desc" } });
	const cmd::result asked = commands.invoke("asset.rename", request);
	BOOST_REQUIRE(asked.code == cmd::codes::confirmation_required);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);

	const std::string ticket(asked.value.at("ticket").as_string());
	BOOST_REQUIRE(commands.approve(ticket));
	request.confirmation = ticket;
	const cmd::result done = commands.invoke("asset.rename", request);
	BOOST_REQUIRE_MESSAGE(done.ok, done.error);
	BOOST_TEST(has("alone.desc"));
	BOOST_TEST(history.size() == 1u);
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(TheAgentPolicyAnswerOfAHeadlessRun_LetsRenameRun_AndRecordsIt_ButNeverDelete)
{
	open();
	policy.set_headless(true);
	policy_dir.write("answers.json", R"({"commands":{"asset.delete":"allow","asset.rename":"allow"}})");
	fs::create_directories(layer.path() / ".git");
	policy.set_preanswers_file(policy_dir.path() / "answers.json", layer.path());
	BOOST_REQUIRE_MESSAGE(policy.preanswers_error().empty(), policy.preanswers_error());
	ask_a_person();
	gate->set_can_ask(false);
	const auto before = snapshot();

	const cmd::result deleted = commands.invoke("asset.delete", agent_call(json::object{ { "path", "lonely.desc" } }));
	BOOST_TEST(deleted.code == cmd::codes::denied);
	BOOST_TEST(deleted.error.find("confirmed") != std::string::npos);
	BOOST_TEST((snapshot() == before));

	const cmd::result moved = commands.invoke("asset.move", agent_call(json::object{ { "path", "lonely.desc" }, { "to", "env" } }));
	BOOST_TEST(moved.code == cmd::codes::denied, "the file does not answer for asset.move");
	BOOST_TEST((snapshot() == before));

	const cmd::result renamed = commands.invoke("asset.rename", agent_call(json::object{ { "path", "lonely.desc" }, { "to", "alone.desc" } }));
	BOOST_TEST(renamed.ok, renamed.error);
	BOOST_TEST(has("alone.desc"));
	BOOST_TEST(history.size() == 1u);
	commands.set_policy_gate(nullptr);
}

BOOST_AUTO_TEST_CASE(AnAgentsCallWithNoQuestionBehindIt_IsNotRun)
{
	open();
	const auto before = snapshot();
	// No gate: nothing described the plan to anyone.
	const cmd::result result = commands.invoke("asset.rename", agent_call(json::object{ { "path", "lonely.desc" }, { "to", "alone.desc" } }));
	BOOST_TEST(result.code == cmd::codes::denied);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(WhatAnAgentIsTold_NamesNoFolderOfTheHost)
{
	open();
	const std::string text = std::format("cannot make {}/levels/x: no, in {}/abc and {}/e.desc, also {}", fs_paths::generic_utf8_of(layer.path()),
		fs_paths::generic_utf8_of(journal.path()), fs_paths::generic_utf8_of(engine.path()), fs_paths::generic_utf8_of(layer.path()));
	const std::string told = operations.without_host_paths(text);
	BOOST_TEST(told == "cannot make res://levels/x: no, in cache://asset_ops/abc and <engine>/e.desc, also res://");
}

BOOST_AUTO_TEST_CASE(TheGuards_AnswerBusyOrFailed_AndNothingMoves)
{
	open();
	const auto before = snapshot();
	const json::object rename{ { "path", "sky.desc" }, { "to", "clouds.desc" } };

	blocked = "a migration is running";
	const cmd::result migrating = call("asset.rename", rename);
	BOOST_TEST(migrating.code == cmd::codes::busy);
	BOOST_TEST(migrating.error.find("a migration is running") != std::string::npos);
	blocked.reset();

	waiting = "the asset index is still being built";
	BOOST_TEST(call("asset.rename", rename).code == cmd::codes::busy);
	waiting.reset();

	project_open = false;
	BOOST_TEST(call("asset.rename", rename).code == cmd::codes::failed);
	project_open = true;

	BOOST_TEST(call("asset.rename", rename, cmd::origin::game()).code == cmd::codes::denied);
	BOOST_TEST(call("asset.rename", json::object{ { "path", "sky.desc" } }).code == cmd::codes::invalid_args, "`to` is required");
	BOOST_TEST(call("asset.rename", json::object{ { "path", "code://a.cpp" }, { "to", "b.cpp" } }).code == cmd::codes::invalid_args);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(ARefusalOfTheOperations_KeepsItsMachineCodeInThePayload)
{
	open();
	const cmd::result taken = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "other.desc" } });
	BOOST_REQUIRE(!taken.ok);
	BOOST_TEST(taken.code == cmd::codes::invalid_args);
	BOOST_TEST(text_of(taken.value.at("code")) == "target_exists");
	BOOST_TEST(taken.error.find("target_exists") != std::string::npos);

	BOOST_TEST(call("asset.rename", json::object{ { "path", "res://nothing.desc" }, { "to", "x.desc" } }).code == cmd::codes::not_found);

	open_files.insert("res://levels/main.desc");
	const cmd::result held = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE(!held.ok);
	BOOST_TEST(held.code == cmd::codes::busy);
	BOOST_TEST(text_of(held.value.at("code")) == "referrer_open");
	BOOST_TEST(has("sky.desc"));
	BOOST_TEST(history.size() == 0u);
}

BOOST_AUTO_TEST_CASE(EveryRefusalCode_HasACommandCode)
{
	const auto code = [](edt::refusal_code refusal) { return std::string_view(edt::command_code_of(refusal)); };
	BOOST_TEST((code(edt::refusal_code::database_not_ready) == cmd::codes::busy));
	BOOST_TEST((code(edt::refusal_code::referrer_open) == cmd::codes::busy));
	BOOST_TEST((code(edt::refusal_code::stale_plan) == cmd::codes::stale));
	BOOST_TEST((code(edt::refusal_code::not_found) == cmd::codes::not_found));
	BOOST_TEST((code(edt::refusal_code::has_referrers) == cmd::codes::needs_input));
	BOOST_TEST((code(edt::refusal_code::usage_scan_skipped) == cmd::codes::needs_input));
	BOOST_TEST((code(edt::refusal_code::engine_layer) == cmd::codes::failed));
	BOOST_TEST((code(edt::refusal_code::target_exists) == cmd::codes::invalid_args));
	BOOST_TEST((code(edt::refusal_code::bad_name) == cmd::codes::invalid_args));
}

BOOST_AUTO_TEST_CASE(AnOperation_TellsTheDatabaseItIsTheEditorsOwnChange)
{
	edt::asset_database& database = open();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	BOOST_TEST(database.find(res::tag::make("clouds.desc")) != nullptr, "known before any watcher reported the move");
	BOOST_TEST(database.find(res::tag::make("sky.desc")) == nullptr);
}

BOOST_AUTO_TEST_CASE(AnUndoThroughTheHistory_PutsEverythingBack_AndDeletesTheJournal)
{
	open();
	const auto before = snapshot();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const std::string id = text_of(result.value.at("operation"));
	BOOST_TEST(fs::is_regular_file(journal.path() / id / "journal.json"));

	BOOST_TEST((history.next_operation_id() == std::optional<std::string>(id)));
	const edt::asset_fix_result undone = history.undo_last(operations);
	BOOST_REQUIRE_MESSAGE(undone.ok, undone.error);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(!fs::exists(journal.path() / id), "put back: the journal folder is not needed any more");
	BOOST_TEST(history.size() == 0u);
}

BOOST_AUTO_TEST_CASE(AnUndoThatFindsAFileChanged_SaysWhichOnes_KeepsTheEntry_AndWorksAgainOnceItIsFixed)
{
	open();
	const auto before = snapshot();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const std::string id = text_of(result.value.at("operation"));
	const std::string original = file_text("levels/main.desc");
	write("levels/main.desc", canonical(R"({ "__type": "level_desc", "edited": true })"));
	const auto after_edit = snapshot();

	const edt::asset_fix_result undone = history.undo_last(operations);
	BOOST_TEST(!undone.ok);
	BOOST_TEST(undone.keep, "the entry stays for another try");
	BOOST_TEST(undone.error.find("Rename sky.desc") != std::string::npos);
	BOOST_TEST(undone.error.find("levels/main.desc") != std::string::npos);
	BOOST_TEST((snapshot() == after_edit), "nothing was touched");
	BOOST_TEST(history.size() == 1u);
	BOOST_TEST(fs::is_regular_file(journal.path() / id / "journal.json"));

	write("levels/main.desc", original);
	const edt::asset_fix_result again = history.undo_last(operations);
	BOOST_REQUIRE_MESSAGE(again.ok, again.error);
	BOOST_TEST((snapshot() == before));
	BOOST_TEST(history.size() == 0u);
}

BOOST_AUTO_TEST_CASE(AnUndoThatKeepsFailing_IsDroppedAfterThreeTries_AndItsJournalIsSetAside)
{
	open();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const std::string id = text_of(result.value.at("operation"));
	write("levels/main.desc", canonical(R"({ "__type": "level_desc", "edited": true })"));
	for (std::size_t attempt = 1; attempt < edt::asset_history::max_undo_attempts; ++attempt) {
		const edt::asset_fix_result kept = history.undo_last(operations);
		BOOST_TEST(!kept.ok);
		BOOST_TEST(kept.keep);
		BOOST_TEST(history.size() == 1u);
	}
	const edt::asset_fix_result dropped = history.undo_last(operations);
	BOOST_TEST(!dropped.ok);
	BOOST_TEST(!dropped.keep);
	BOOST_TEST(history.size() == 0u);
	BOOST_TEST(fs::is_regular_file(journal.path() / id / "journal.dropped.json"));
	BOOST_TEST(!fs::exists(journal.path() / id / "journal.json"), "a restart does not bring a dropped entry back");
	edt::asset_history restarted = make_history();
	BOOST_TEST(restarted.restore(roots()) == 0u);
}

BOOST_AUTO_TEST_CASE(AnUndoOfAFileThatIsOpenWithUnsavedChanges_KeepsTheEntry_AndWorksWhenItIsSaved)
{
	open();
	const auto before = snapshot();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	open_files = { "res://clouds.desc" };
	const edt::asset_fix_result blocked = history.undo_last(operations);
	BOOST_TEST(!blocked.ok);
	BOOST_TEST(blocked.keep);
	BOOST_TEST(history.size() == 1u);
	open_files.clear();
	BOOST_TEST(history.undo_last(operations).ok);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(AfterARestart_TheHistoryIsRebuiltFromTheJournals_AndADeleteComesBack)
{
	open();
	const auto before = snapshot();
	const cmd::result renamed = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(renamed.ok, renamed.error);
	settle();
	const cmd::result removed = call("asset.delete", json::object{ { "path", "lonely.desc" } });
	BOOST_REQUIRE_MESSAGE(removed.ok, removed.error);

	edt::asset_history restarted = make_history(100);
	BOOST_TEST(restarted.restore(operations.changeable_roots()) == 2u);
	BOOST_TEST((restarted.next_label() == std::optional<std::string>("Delete lonely.desc")));
	BOOST_TEST((restarted.next_operation_id() == std::optional<std::string>(text_of(removed.value.at("operation")))));
	BOOST_TEST((restarted.next_serial() == std::optional<std::uint64_t>(102u)), "numbered oldest first");

	const edt::asset_fix_result first = restarted.undo_last(operations);
	BOOST_REQUIRE_MESSAGE(first.ok, first.error);
	BOOST_TEST(has("lonely.desc"));
	BOOST_TEST((restarted.next_label() == std::optional<std::string>("Rename sky.desc")));
	const edt::asset_fix_result second = restarted.undo_last(operations);
	BOOST_REQUIRE_MESSAGE(second.ok, second.error);
	BOOST_TEST((snapshot() == before));
}

BOOST_AUTO_TEST_CASE(TheRebuild_KeepsTheNewestTwenty_AndLeavesWhatTheEditorDidNotList)
{
	open();
	const auto made = [&](const std::string& id, std::vector<edt::journal_step> steps) {
		edt::operation_journal entry;
		entry.id = id;
		entry.operation = edt::asset_operation::rename;
		entry.directory = fs_paths::generic_utf8_of(journal.path() / id);
		entry.steps = std::move(steps);
		journal.write(id + "/journal.json", json::serialize(edt::journal_to_json(entry)));
		return entry;
	};
	const std::string root = fs_paths::generic_utf8_of(layer.path());
	for (int i = 0; i < 22; ++i) {
		history.record(made(std::format("op{:02}", i), { edt::journal_step{ edt::journal_step_kind::move, root, "a.desc", "b.desc", "h", "", "" } }));
	}
	history.record(made("empty", {}));
	history.record(made("elsewhere", { edt::journal_step{ edt::journal_step_kind::move, "/somewhere/else", "a.desc", "b.desc", "h", "", "" } }));
	history.record(made("engine", { edt::journal_step{ edt::journal_step_kind::move, fs_paths::generic_utf8_of(engine.path()), "a.desc", "b.desc", "h", "", "" } }));
	journal.write("garbage/journal.json", "this is not json");
	journal.write("plain_file.txt", "x");
	made("unlisted", { edt::journal_step{ edt::journal_step_kind::move, root, "a.desc", "b.desc", "h", "", "" } });

	edt::asset_history restarted = make_history();
	// The list holds the newest twenty records: op05..op21 and the three after them; of those the empty journal and the ones that
	// name a folder that is not the project's own are no entries. `unlisted` and the old ones never were.
	BOOST_TEST(restarted.restore(operations.changeable_roots()) == 17u);
	BOOST_TEST((restarted.next_operation_id() == std::optional<std::string>("op21")));
	BOOST_TEST((restarted.next_label() == std::optional<std::string>("Rename a.desc")));
	BOOST_TEST(fs::is_regular_file(journal.path() / "op00" / "journal.json"), "older ones are left on the disk");
	BOOST_TEST(fs::is_regular_file(journal.path() / "unlisted" / "journal.json"));

	edt::asset_history none{ journal.path() / "nowhere", {}, [] { return std::uint64_t{ 1 }; } };
	BOOST_TEST(none.restore(roots()) == 0u);
}

BOOST_AUTO_TEST_CASE(AJournalThatIsNotInTheEditorsOwnList_IsNeverRestoredNorUndone)
{
	open();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const std::string id = text_of(result.value.at("operation"));
	BOOST_TEST(fs::is_regular_file(list_file()));
	BOOST_TEST(!list_file().generic_string().starts_with(layer.path().generic_string() + "/"));

	// A journal that came with the project's cache folder: well formed, naming the project's folder, but nobody listed it.
	std::ifstream real(journal.path() / id / "journal.json", std::ios::binary);
	json::value forged = json::parse(std::string(std::istreambuf_iterator<char>(real), std::istreambuf_iterator<char>()));
	real.close();
	forged.as_object()["id"] = "forged01";
	forged.as_object()["directory"] = fs_paths::generic_utf8_of(journal.path() / "forged01");
	journal.write("forged01/journal.json", json::serialize(forged));

	edt::asset_history restarted = make_history();
	BOOST_TEST(restarted.restore(roots()) == 1u, "only the listed journal");
	BOOST_TEST((restarted.next_operation_id() == std::optional<std::string>(id)));

	fs::remove(list_file());
	edt::asset_history without_list = make_history();
	BOOST_TEST(without_list.restore(roots()) == 0u, "no list: no journal is trusted");
	BOOST_TEST(!without_list.undo_last(operations).ok);
}

BOOST_AUTO_TEST_CASE(AListedJournalThatWasChangedSince_IsIgnored_AndTheLabelIsTheLists)
{
	open();
	const cmd::result result = call("asset.rename", json::object{ { "path", "sky.desc" }, { "to", "clouds.desc" } });
	BOOST_REQUIRE_MESSAGE(result.ok, result.error);
	const std::string id = text_of(result.value.at("operation"));

	{
		std::ifstream in(list_file(), std::ios::binary);
		json::value listed = json::parse(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
		listed.as_object()["entries"].as_array()[0].as_object()["label"] = "Tidy the materials";
		in.close();
		std::ofstream(list_file(), std::ios::binary | std::ios::trunc) << json::serialize(listed);
	}
	edt::asset_history restarted = make_history();
	BOOST_REQUIRE(restarted.restore(roots()) == 1u);
	BOOST_TEST((restarted.next_label() == std::optional<std::string>("Tidy the materials")), "the label is the editor's list's, not the journal's");

	const fs::path journal_file = journal.path() / id / "journal.json";
	std::string text;
	{
		std::ifstream in(journal_file, std::ios::binary);
		text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	json::value changed = json::parse(text);
	changed.as_object()["steps"].as_array()[0].as_object()["hash"] = "fnv1a64:0000000000000000";
	std::ofstream(journal_file, std::ios::binary | std::ios::trunc) << json::serialize(changed);
	edt::asset_history tampered = make_history();
	BOOST_TEST(tampered.restore(roots()) == 0u, "the journal is not what the editor listed");
}

BOOST_AUTO_TEST_CASE(TheHistoryIsKeptToTwentyEntries_AndAnOperationIsNewerThanTheFixesBeforeIt)
{
	open();
	for (int i = 0; i < 22; ++i) {
		edt::operation_journal entry;
		entry.id = std::format("r{}", i);
		entry.steps.push_back(edt::journal_step{ edt::journal_step_kind::move, "x", "a.desc", "b.desc", "h", "", "" });
		history.record(std::move(entry));
	}
	BOOST_TEST(history.size() == edt::asset_history::max_entries);
	BOOST_TEST((history.next_operation_id() == std::optional<std::string>("r21")));

	BOOST_TEST(edt::asset_history::newer_than(5, 3));
	BOOST_TEST(!edt::asset_history::newer_than(3, 5));
	BOOST_TEST(edt::asset_history::newer_than(1, std::nullopt));
	BOOST_TEST(!edt::asset_history::newer_than(std::nullopt, 7));
	BOOST_TEST(!edt::asset_history::newer_than(std::nullopt, std::nullopt));
	BOOST_TEST((history.next_serial().value() > db->next_undo_serial().value_or(0)), "serials come from the database's counter");
}

BOOST_AUTO_TEST_CASE(ADocumentWithUnsavedChanges_OrTheOpenLevel_BlocksAnOperation)
{
	fake_document scene{ "level", false };
	fake_document clean{ "clean", false };
	fake_document dirty{ "dirty", true };
	BOOST_TEST(edt::is_open_with_unsaved_changes(&dirty, scene, false), "a dirty data or prefab document");
	BOOST_TEST(!edt::is_open_with_unsaved_changes(&clean, scene, false), "a clean one has nothing to lose");
	BOOST_TEST(!edt::is_open_with_unsaved_changes(nullptr, scene, false), "not open");
	BOOST_TEST(edt::is_open_with_unsaved_changes(&scene, scene, false), "the open level takes no change from outside, clean or not");
	BOOST_TEST(edt::is_open_with_unsaved_changes(nullptr, scene, true), "the editor copy of the open level");
}

BOOST_AUTO_TEST_SUITE_END()
