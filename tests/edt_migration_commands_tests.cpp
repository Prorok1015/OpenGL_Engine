#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "edt_agent_policy.h"
#include "edt_migration_commands.h"
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_job_service.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// project.migration_plan / project.migrate (#759, EPIC-58 S7) over fake editor hooks, on copies of the fixture of old
// versions (#750): the plan writes nothing, the migration runs only after the plan a person was shown, never with
// unsaved changes or while the editor is busy, and answers with the report of the command line.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
namespace pmg = prj::migration;

std::string read_text(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_text(const fs::path& path, const std::string& text)
{
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary) << text;
}

// Every file of the tree, hidden ones (.snk, .cache) included; a link is not followed.
std::map<std::string, std::string> snapshot(const fs::path& root)
{
	std::map<std::string, std::string> files;
	for (const auto& entry : fs::recursive_directory_iterator(root)) {
		const fs::file_status status = entry.symlink_status();
		std::string content = "<dir>";
		if (fs::is_symlink(status)) content = "<link>";
		else if (fs::is_regular_file(status)) content = read_text(entry.path());
		files[fs::relative(entry.path(), root).generic_string()] = std::move(content);
	}
	return files;
}

struct migration_fixture : test_util::scene_env
{
	migration_fixture()
	{
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ copy.res() });

		edt::migration_deps deps{ jobs };
		deps.project_root = [this]() -> std::optional<fs::path> {
			if (!project_open) return std::nullopt;
			return copy.root();
		};
		deps.project_name = [] { return std::string{ "MigrationFixtureV1" }; };
		deps.content_roots = [this] { return std::vector<fs::path>{ copy.res() }; };
		deps.descs = [this]() -> desc::desc_system& { return *descs; };
		deps.resources = [this]() -> res::resource_system& { return *rs; };
		deps.busy_reason = [this] { return busy; };
		deps.unsaved_reason = [this] { return unsaved; };
		deps.close_documents_and_detach_level = [this] {
			closed = true;
			return current_level;
		};
		deps.signal_changed = [this](const res::tag& tag) { signaled.emplace_back(tag.view()); };
		deps.rebuild_assets = [this] { ++rebuilds; };
		deps.reopen_level = [this](const res::tag& level, std::function<void(bool)> loaded) -> std::optional<std::string> {
			attached = true;
			if (!level.is_valid()) return std::nullopt;
			loading = true;
			auto frames = std::make_shared<int>(2);
			implicit.emplace_back("edt/load_level", [this, level, loaded, frames] {
				if (--*frames > 0) return true;
				loading = false;
				if (load_succeeds) current_level = level;
				loaded(load_succeeds);
				return false;
			});
			return std::nullopt;
		};
		deps.level_load_in_progress = [this] { return loading; };
		deps.current_level_tag = [this] { return current_level; };
		deps.register_implicit = [this](std::string_view id, std::function<bool()> fn) { implicit.emplace_back(std::string(id), std::move(fn)); };
		registrations = edt::register_migration_commands(commands, std::move(deps));
	}

	~migration_fixture()
	{
		implicit.clear();
		registrations.clear();
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	cmd::result call(const std::string& id, json::object args = {}, cmd::origin from = cmd::origin::agent("s1"))
	{
		return commands.invoke(id, json::value(std::move(args)), std::move(from));
	}

	// What the registry does when the policy asks a person: the text they are shown.
	std::string ask(cmd::origin from = cmd::origin::agent("s1"))
	{
		const cmd::command_desc* desc = commands.find("project.migrate");
		BOOST_REQUIRE(desc && desc->describe);
		cmd::call request;
		request.from = std::move(from);
		return desc->describe(request).value_or(std::string{});
	}

	// The frames of the editor until no callback asks for another one.
	void run_frames()
	{
		for (int frame = 0; frame < 32 && !implicit.empty(); ++frame) {
			const auto callbacks = implicit;
			std::vector<std::string> ended;
			for (const auto& [id, callback] : callbacks) {
				if (!callback()) ended.push_back(id);
			}
			for (const std::string& id : ended) std::erase_if(implicit, [&id](const auto& entry) { return entry.first == id; });
		}
		BOOST_TEST(implicit.empty());
	}

	// Starts project.migrate, runs the frames and the job; the job's result.
	json::object migrate(cmd::origin from = cmd::origin::agent("s1"))
	{
		const cmd::result started = call("project.migrate", {}, std::move(from));
		BOOST_REQUIRE_MESSAGE(started.ok, started.code << ": " << started.error);
		const std::uint64_t job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
		run_frames();
		test_util::pump(job_tasks);
		const auto info = jobs.info(job);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE((info->status == edt::job_status::done));
		BOOST_REQUIRE(info->result.has_value());
		BOOST_REQUIRE(info->result->is_object());
		return info->result->as_object();
	}

	static std::string error_code_of(const json::object& result)
	{
		const json::value* error = result.if_contains("error");
		return error ? std::string(error->at("code").as_string()) : std::string{};
	}

	test_util::fixture_copy copy{ "migration_v1" };
	ds::task_system job_tasks{ test_util::manual_tasks() };
	edt::job_service jobs{ job_tasks };
	cmd::command_registry commands;

	bool project_open = true;
	std::optional<std::string> busy;
	std::optional<std::string> unsaved;
	res::tag current_level;
	bool closed = false;
	bool attached = false;
	bool loading = false;
	bool load_succeeds = true;
	int rebuilds = 0;
	std::vector<std::string> signaled;
	std::vector<std::pair<std::string, std::function<bool()>>> implicit;
	std::vector<edt::registration> registrations;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMigrationCommands, migration_fixture)

// What --dry-run prints and exits with, as data: nothing is written, whatever the plan holds.
BOOST_AUTO_TEST_CASE(ThePlan_IsReadOnly_AndItsCodeIsTheCLIs)
{
	const auto before = snapshot(copy.root());
	const cmd::result refused = call("project.migration_plan");
	BOOST_REQUIRE_MESSAGE(refused.ok, refused.error);
	BOOST_TEST(refused.value.at("code").as_int64() == pmg::k_exit_refused);
	BOOST_TEST(refused.value.at("plan").as_object().contains("refused"));
	BOOST_TEST((snapshot(copy.root()) == before));

	fs::remove(copy.res() / "broken.desc");
	const auto without_broken = snapshot(copy.root());
	const cmd::result planned = call("project.migration_plan");
	BOOST_REQUIRE_MESSAGE(planned.ok, planned.error);
	BOOST_TEST(planned.value.at("code").as_int64() == pmg::k_exit_migration_needed);
	const json::object& plan = planned.value.at("plan").as_object();
	BOOST_TEST(!plan.contains("refused"));
	BOOST_TEST(!plan.at("files").as_array().empty());
	const json::array& files = plan.at("files").as_array();
	const bool camera = std::ranges::any_of(files, [](const json::value& file) { return file.at("path").as_string() == "res/cameras/cam_v1.desc"; });
	BOOST_TEST(camera);
	const json::array& steps = plan.at("project_steps").as_array();
	BOOST_TEST(std::ranges::any_of(steps, [](const json::value& step) { return step.at("id").as_string() == "assets.ids"; }));
	BOOST_TEST(plan.at("manifest").at("outdated").as_bool());
	BOOST_TEST(!plan.at("skipped").as_array().empty());
	BOOST_TEST(std::string(planned.value.at("text").as_string()).find("assets.ids") != std::string::npos);
	BOOST_TEST((snapshot(copy.root()) == without_broken));
}

BOOST_AUTO_TEST_CASE(BothCommands_TakeNoArgument)
{
	for (const char* id : { "project.migration_plan", "project.migrate" }) {
		for (const json::object& args : { json::object{ { "force", true } }, json::object{ { "path", "x" } }, json::object{ { "project", "x" } } }) {
			BOOST_TEST_CONTEXT(id << " " << json::serialize(args)) {
				BOOST_TEST(call(id, args).code == cmd::codes::invalid_args);
			}
		}
	}
}

BOOST_AUTO_TEST_CASE(ThePolicy_ReadsThePlan_AndAsksForTheMigration)
{
	const cmd::command_desc* plan = commands.find("project.migration_plan");
	const cmd::command_desc* migrate = commands.find("project.migrate");
	BOOST_REQUIRE(plan);
	BOOST_REQUIRE(migrate);
	BOOST_TEST((plan->category == cmd::agent_category::read));
	BOOST_TEST(!plan->mutates);
	BOOST_TEST((migrate->category == cmd::agent_category::project_migrate));
	BOOST_TEST((migrate->risk == cmd::danger::confirm));
	BOOST_TEST(migrate->mutates);
	BOOST_TEST(migrate->starts_job);
	BOOST_TEST((edt::default_mode(cmd::agent_category::project_migrate) == cmd::policy_mode::ask));
	BOOST_TEST((edt::parse_settable_category("project_migrate") == cmd::agent_category::project_migrate));
}

BOOST_AUTO_TEST_CASE(WithoutAProject_BothAnswerNoProject)
{
	project_open = false;
	BOOST_TEST(call("project.migration_plan").code == "no_project");
	BOOST_TEST(call("project.migrate").code == "no_project");
}

// The person approves the plan describe showed: a call nobody was shown is refused and writes nothing.
BOOST_AUTO_TEST_CASE(AMigration_WithoutTheQuestion_IsDenied)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	BOOST_TEST(call("project.migrate").code == cmd::codes::denied);
	// The plan shown to another session is not this one's.
	static_cast<void>(ask(cmd::origin::agent("s2")));
	BOOST_TEST(call("project.migrate").code == cmd::codes::denied);
	BOOST_TEST(implicit.empty());
	BOOST_TEST((snapshot(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(TheQuestion_NamesTheProject_WhatIsLeft_AndTheLevel)
{
	const std::string text = ask();
	BOOST_TEST(text.find("MigrationFixtureV1") != std::string::npos, text);
	BOOST_TEST(text.find("will be refused") != std::string::npos, text);
	BOOST_TEST(text.find("newer.desc") != std::string::npos, text);
	BOOST_TEST(text.find("opened again from disk") != std::string::npos, text);
}

// A byte changed between the question and the answer: the person approved another plan.
BOOST_AUTO_TEST_CASE(AProjectChangedAfterTheQuestion_IsDenied_AndNothingIsWritten)
{
	fs::remove(copy.res() / "broken.desc");
	static_cast<void>(ask());
	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	const json::object result = migrate();
	BOOST_TEST(error_code_of(result) == cmd::codes::denied);
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == changed));
}

BOOST_AUTO_TEST_CASE(AMigration_WritesTheReport_AndOpensTheLevelAgain)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	// newer.desc and the file of an unloaded module stay: partial, as the command line says.
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
	BOOST_TEST(std::string(result.at("status").as_string()) == "partial");
	const std::string id(result.at("id").as_string());
	BOOST_TEST(!id.empty());
	const json::object& report = result.at("report").as_object();
	for (const char* key : { "id", "status", "files", "skipped", "created_files", "backup" }) {
		BOOST_TEST_CONTEXT(key) {
			BOOST_TEST(report.contains(key));
		}
	}
	BOOST_TEST(fs::exists(copy.root() / ".snk" / "migrations" / id / "report.json"));
	BOOST_TEST(!std::string(result.at("text").as_string()).empty());
	BOOST_TEST(closed);
	BOOST_TEST(attached);
	BOOST_TEST(rebuilds == 1);
	BOOST_TEST((std::ranges::find(signaled, std::string{ "res://cameras/cam_v1.desc" }) != signaled.end()));
	const json::object& level = result.at("level").as_object();
	BOOST_TEST(std::string(level.at("tag").as_string()) == "res://levels/old.desc");
	BOOST_TEST(level.at("reopened").as_bool());

	// Nothing is left to do but what never migrates.
	const cmd::result again = call("project.migration_plan");
	BOOST_TEST(again.value.at("code").as_int64() == pmg::k_exit_partial);
}

BOOST_AUTO_TEST_CASE(AProjectThatMigratesWhole_IsDone_WithoutALevel)
{
	fs::remove(copy.res() / "broken.desc");
	fs::remove(copy.res() / "newer.desc");
	fs::remove(copy.res() / "module/unknown.desc");
	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_done);
	BOOST_TEST(attached);
	const json::object& level = result.at("level").as_object();
	BOOST_TEST(!level.at("reopened").as_bool());
	BOOST_TEST(level.contains("reason"));
}

// A level that does not load again is reported; the migration stands.
BOOST_AUTO_TEST_CASE(ALevelThatDoesNotLoadAgain_IsReported)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	load_succeeds = false;
	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
	BOOST_TEST(!result.at("level").at("reopened").as_bool());
	BOOST_TEST(result.at("level").as_object().contains("reason"));
}

// An invalid file refuses the whole migration (no --force for agents): nothing is closed or written.
BOOST_AUTO_TEST_CASE(AnInvalidFile_RefusesTheMigration_Code2)
{
	const auto before = snapshot(copy.root());
	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_refused);
	BOOST_TEST(result.contains("refused"));
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == before));
}

BOOST_AUTO_TEST_CASE(UnsavedChanges_NeedInput_AndABusyEditor_IsBusy)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	static_cast<void>(ask());
	unsaved = "the level has unsaved changes";
	BOOST_TEST(call("project.migrate").code == cmd::codes::needs_input);
	unsaved.reset();
	busy = "Play is running";
	BOOST_TEST(call("project.migrate").code == cmd::codes::busy);
	busy = "a level is loading";
	BOOST_TEST(call("project.migrate").code == cmd::codes::busy);

	// Busy again between the start and the frame that writes: the job says so, nothing is written.
	busy.reset();
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	busy = "Play is running";
	run_frames();
	test_util::pump(job_tasks);
	const auto info = jobs.info(static_cast<std::uint64_t>(started.value.at("job").as_int64()));
	BOOST_REQUIRE(info && info->result);
	BOOST_TEST(error_code_of(info->result->as_object()) == cmd::codes::busy);
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == before));
}

// A second migration while one runs is busy.
BOOST_AUTO_TEST_CASE(OneMigrationAtATime)
{
	fs::remove(copy.res() / "broken.desc");
	static_cast<void>(ask());
	const cmd::result first = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(first.ok, first.error);
	static_cast<void>(ask());
	BOOST_TEST(call("project.migrate").code == cmd::codes::busy);
	run_frames();
	test_util::pump(job_tasks);
}

// res/ is a link out of the project: the plan and the migration refuse it, nothing is written on either side.
BOOST_AUTO_TEST_CASE(AContentRootLinkedOutOfTheProject_IsRefused)
{
	test_util::temp_dir outside{ "edt_migration_commands_outside" };
	write_text(outside.path() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc"));
	fs::remove_all(copy.res());
	std::error_code link_error;
	fs::create_directory_symlink(outside.path(), copy.res(), link_error);
	if (link_error) {
		// Windows without the right to create links.
		BOOST_TEST_MESSAGE("symlinks unavailable: " << link_error.message());
		return;
	}
	const auto inside = snapshot(copy.root());
	const auto out = snapshot(outside.path());
	const cmd::result planned = call("project.migration_plan");
	BOOST_REQUIRE_MESSAGE(planned.ok, planned.error);
	BOOST_TEST(planned.value.at("code").as_int64() == pmg::k_exit_refused);

	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_refused);
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == inside));
	BOOST_TEST((snapshot(outside.path()) == out));
}

BOOST_AUTO_TEST_SUITE_END()
