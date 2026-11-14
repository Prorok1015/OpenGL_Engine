#include <boost/test/unit_test.hpp>
#include "test_fixture_project.hpp"
#include "test_manual_tasks.hpp"
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "test_watchdog.hpp"
#include "edt_agent_policy.h"
#include "edt_migration_commands.h"
#include <snk/command/cmd_registry.h>
#include <snk/editor/edt_job_service.h>
#include <snk/project/prj_json_text.h>
#include <snk/project_migration/pmg_command.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <boost/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

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

// The editor's pool: the job (and the apply in it) runs on an io thread of its own while the test thread runs the frames.
ds::task_system_config job_pool()
{
	ds::task_system_config config;
	config.mode = ds::policy::threaded;
	config.io_threads = 1;
	config.compute_threads = 1;
	return config;
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
			// A cancel that lands between the detach and the rollback's commit point.
			if (cancel_on_detach) cancelled_on_detach = jobs.cancel(*cancel_on_detach);
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
		deps.now = [this] { return clock; };
		deps.approved_internally = [this](std::string_view ticket) { return internal_tickets.contains(std::string(ticket)); };
		deps.code_identity = [this] { return code_identity; };
		deps.level_left_detached = [this](const res::tag& level, std::function<bool()> rolled_back, const std::string& reason) {
			left_detached = reason;
			left_detached_level = level;
			left_detached_rolled_back = std::move(rolled_back);
		};
		deps.apply = [this](const pmg::migration_plan& plan, const pmg::apply_context& context) {
			return apply ? apply(plan, context) : pmg::apply_migration(plan, context);
		};
		deps.running_changed = [this](bool running) { running_said.push_back(running); };
		// As the editor: the save block of a stopped migration goes once its rolled_back() holds.
		deps.migration_rolled_back = [this](const std::string& id) {
			rolled_back_ids.push_back(id);
			if (left_detached && left_detached_rolled_back && left_detached_rolled_back()) left_detached.reset();
		};
		deps.rollback = [this](const fs::path& root, std::string_view id, const pmg::rollback_context& context) {
			return rollback ? rollback(root, id, context) : pmg::rollback_migration(root, id, context);
		};
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

	// One frame of the editor: every implicit callback once, then what the job queued for the main thread.
	void run_frame()
	{
		const auto callbacks = implicit;
		std::vector<std::string> ended;
		for (const auto& [id, callback] : callbacks) {
			if (!callback()) ended.push_back(id);
		}
		for (const std::string& id : ended) std::erase_if(implicit, [&id](const auto& entry) { return entry.first == id; });
		job_tasks.drain_main(std::chrono::milliseconds(1));
	}

	// The frames of the editor until no callback asks for another one; the apply runs on the job's thread meanwhile.
	void run_frames()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!implicit.empty() && std::chrono::steady_clock::now() < deadline) {
			run_frame();
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
		BOOST_TEST(implicit.empty());
	}

	// Until the job service has no job running: its end is delivered on the main thread.
	void settle_jobs()
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!jobs.list().empty() && std::chrono::steady_clock::now() < deadline) {
			job_tasks.drain_main(std::chrono::milliseconds(1));
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		BOOST_TEST(jobs.list().empty());
	}

	// Starts project.migrate, runs the frames and the job; the job's result.
	json::object migrate(cmd::origin from = cmd::origin::agent("s1"))
	{
		return job_result(call("project.migrate", {}, std::move(from)));
	}

	// The frames and the job of a project.migrate that started.
	json::object job_result(const cmd::result& started)
	{
		BOOST_REQUIRE_MESSAGE(started.ok, started.code << ": " << started.error);
		const std::uint64_t job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
		run_frames();
		settle_jobs();
		const auto info = jobs.info(job);
		BOOST_REQUIRE(info.has_value());
		BOOST_REQUIRE((info->status == edt::job_status::done));
		BOOST_REQUIRE(info->result.has_value());
		BOOST_REQUIRE(info->result->is_object());
		return info->result->as_object();
	}

	// What a person is shown before an agent's rollback of `id`.
	std::string ask_rollback(const std::string& id, cmd::origin from = cmd::origin::agent("s1"))
	{
		const cmd::command_desc* desc = commands.find("project.migrate_rollback");
		BOOST_REQUIRE(desc && desc->describe);
		cmd::call request;
		request.args = json::object{ { "id", id } };
		request.from = std::move(from);
		return desc->describe(request).value_or(std::string{});
	}

	json::object roll_back(const std::string& id, cmd::origin from = cmd::origin::ui())
	{
		return job_result(call("project.migrate_rollback", { { "id", id } }, std::move(from)));
	}

	// A migration of the fixture (without its invalid file) by a person: its id.
	std::string migrated()
	{
		fs::remove(copy.res() / "broken.desc");
		const json::object result = migrate(cmd::origin::ui());
		BOOST_REQUIRE_MESSAGE(error_code_of(result).empty(), json::serialize(result));
		return std::string(result.at("id").as_string());
	}

	// The project without its journals.
	std::map<std::string, std::string> content()
	{
		auto files = snapshot(copy.root());
		std::erase_if(files, [](const auto& entry) { return entry.first.starts_with(".snk"); });
		return files;
	}

	static std::string error_code_of(const json::object& result)
	{
		const json::value* error = result.if_contains("error");
		return error ? std::string(error->at("code").as_string()) : std::string{};
	}

	// First: destroyed last, so a teardown that hangs (a job waiting for the main thread) aborts too.
	test_util::watchdog guard{ "an EdtMigrationCommands test", std::chrono::seconds(300) };
	test_util::fixture_copy copy{ "migration_v1" };
	ds::task_system job_tasks{ job_pool() };
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
	std::function<pmg::apply_outcome(const pmg::migration_plan&, const pmg::apply_context&)> apply;
	std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::now();
	std::set<std::string> internal_tickets;
	std::uint64_t code_identity = 0;
	std::optional<std::string> left_detached;
	res::tag left_detached_level;
	std::function<bool()> left_detached_rolled_back;
	std::vector<bool> running_said;
	std::vector<std::string> rolled_back_ids;
	std::optional<std::uint64_t> cancel_on_detach;
	bool cancelled_on_detach = false;
	std::function<std::optional<std::string>(const fs::path&, std::string_view, const pmg::rollback_context&)> rollback;
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

// An apply that throws leaves the project in an unknown state: code 6, the rollback named, every planned file announced,
// and the level stays detached (neither loaded from the half migrated files nor saved over them).
BOOST_AUTO_TEST_CASE(AnApplyThatThrows_IsCode6_AndTheLevelStaysDetached)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	std::atomic<bool> held_during_apply{ false };
	apply = [this, &held_during_apply](const pmg::migration_plan&, const pmg::apply_context& context) -> pmg::apply_outcome {
		held_during_apply = rs->changes_held();
		write_text(context.root / ".snk/migrations/test-stopped/report.json", R"({"status":"in_progress"})");
		throw std::runtime_error("injected apply failure");
	};
	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_rollback_failed, json::serialize(result));
	const std::string error(result.at("error").as_string());
	BOOST_TEST(error.find("injected apply failure") != std::string::npos, error);
	BOOST_TEST(error.find("--migrate-rollback test-stopped") != std::string::npos, error);
	const json::object& level = result.at("level").as_object();
	BOOST_TEST(!level.at("reopened").as_bool());
	BOOST_TEST(std::string(level.at("reason").as_string()).find("--migrate-rollback test-stopped") != std::string::npos);
	BOOST_TEST(closed);
	BOOST_TEST(!attached);
	BOOST_TEST(!loading);
	BOOST_REQUIRE(left_detached.has_value());
	BOOST_TEST(left_detached->find("--migrate-rollback test-stopped") != std::string::npos, *left_detached);
	BOOST_TEST(left_detached_level.view() == "res://levels/old.desc");
	// The saves stay blocked until this very migration is rolled back.
	BOOST_REQUIRE(left_detached_rolled_back);
	BOOST_TEST(!left_detached_rolled_back());
	write_text(copy.root() / ".snk/migrations/test-stopped/report.json", R"({"status":"rolled_back"})");
	BOOST_TEST(left_detached_rolled_back());
	BOOST_TEST(rebuilds == 1);
	BOOST_TEST((std::ranges::find(signaled, std::string{ "res://cameras/cam_v1.desc" }) != signaled.end()));
	// The changes were held while it ran and let go before the announcement, the exception notwithstanding.
	BOOST_TEST(held_during_apply.load());
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST((running_said == std::vector<bool>{ true, false }));
}

// #761: the apply runs on the job's io thread while the frames go on; the engine's loads of a desc hop to the main thread
// (the frame callback pumps them), the mounts' reports are held meanwhile, and the editor is told a migration runs (its
// Play, saves, builds, reloads and exit are refused then). The answer is the one of an apply on the main thread.
BOOST_AUTO_TEST_CASE(TheApply_RunsOnTheIoThread_ItsEngineLoadsHopToTheMainThread)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	const std::thread::id main_thread = std::this_thread::get_id();
	std::atomic<bool> on_io{ false };
	std::atomic<bool> held{ false };
	std::atomic<bool> told_running{ false };
	std::atomic<int> hops{ 0 };
	std::atomic<int> hops_on_main{ 0 };
	apply = [&, main_thread](const pmg::migration_plan& plan, const pmg::apply_context& context) {
		on_io = std::this_thread::get_id() != main_thread;
		held = rs->changes_held();
		told_running = running_said == std::vector<bool>{ true };
		// No Boost.Test assertion off the test thread: a missing hook fails the run instead.
		if (!context.on_main) throw std::runtime_error("the apply got no on_main");
		pmg::apply_context counted = context;
		counted.on_main = [&, main_thread, inner = context.on_main](const std::function<void()>& fn) {
			++hops;
			return inner([&, main_thread] {
				if (std::this_thread::get_id() == main_thread) ++hops_on_main;
				fn();
			});
		};
		return pmg::apply_migration(plan, counted);
	};
	static_cast<void>(ask());
	const json::object result = migrate();
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
	BOOST_TEST(std::string(result.at("status").as_string()) == "partial");
	BOOST_TEST(fs::exists(copy.root() / ".snk" / "migrations" / std::string(result.at("id").as_string()) / "report.json"));
	BOOST_TEST(result.at("level").at("reopened").as_bool());
	BOOST_TEST((std::ranges::find(signaled, std::string{ "res://cameras/cam_v1.desc" }) != signaled.end()));
	BOOST_TEST(on_io.load());
	BOOST_TEST(held.load());
	BOOST_TEST(told_running.load());
	BOOST_TEST(hops.load() > 0);
	BOOST_TEST(hops_on_main.load() == hops.load());
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST((running_said == std::vector<bool>{ true, false }));
}

// The editor goes away while the apply waits for the main thread: the hop is refused at once (no hang), the apply ends,
// the hold goes, and the job ends without an answer of success.
BOOST_AUTO_TEST_CASE(TheEditorGoingAway_MidRun_RefusesTheHops_AndTheJobEnds)
{
	fs::remove(copy.res() / "broken.desc");
	std::atomic<bool> entered{ false };
	std::atomic<bool> frames_stopped{ false };
	std::atomic<int> refused{ 0 };
	apply = [&](const pmg::migration_plan&, const pmg::apply_context& context) {
		entered = true;
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!frames_stopped && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (!context.on_main([] {})) ++refused;
		// A refused hop fails the real apply, which then rolls back; here nothing was written.
		return pmg::apply_outcome{};
	};
	static_cast<void>(ask());
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!entered && std::chrono::steady_clock::now() < deadline) {
		run_frame();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	BOOST_REQUIRE(entered.load());
	BOOST_TEST(rs->changes_held());
	frames_stopped = true;
	implicit.clear();
	registrations.clear();
	BOOST_TEST(refused.load() == 1);
	BOOST_TEST(!rs->changes_held());
	settle_jobs();
	const auto info = jobs.info(job);
	BOOST_REQUIRE(info.has_value());
	if (info->status == edt::job_status::done) {
		BOOST_REQUIRE(info->result && info->result->is_object());
		BOOST_TEST(error_code_of(info->result->as_object()) == cmd::codes::failed);
	} else {
		BOOST_TEST((info->status == edt::job_status::cancelled));
	}
}

// The order the editor quits in (#761 review): editor_init and ~editor_system call job_service::cancel_all_and_wait before
// the registrations go. The stop of the job refuses the hop the apply waits on, so the wait ends instead of hanging.
BOOST_AUTO_TEST_CASE(QuittingMidHop_CancelAllAndWaitFirst_RefusesTheHop_ThenTheRegistrationsGo)
{
	fs::remove(copy.res() / "broken.desc");
	std::atomic<bool> entered{ false };
	std::atomic<bool> frames_stopped{ false };
	std::atomic<int> refused{ 0 };
	apply = [&](const pmg::migration_plan&, const pmg::apply_context& context) {
		entered = true;
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!frames_stopped && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		// No frame runs any more: only the job's stop can answer this hop.
		if (!context.on_main([] {})) ++refused;
		return pmg::apply_outcome{};
	};
	static_cast<void>(ask());
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!entered && std::chrono::steady_clock::now() < deadline) {
		run_frame();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	BOOST_REQUIRE(entered.load());
	frames_stopped = true;
	// Let the apply reach its hop, so the stop finds it waiting (a hop after the stop is refused at once anyway).
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	jobs.cancel_all_and_wait();
	BOOST_TEST(refused.load() == 1);
	BOOST_TEST(jobs.list().empty());
	implicit.clear();
	registrations.clear();
	BOOST_TEST(!rs->changes_held());
}

// A job cancelled while it still waits for its io worker never runs its body: the apply handed to it is never taken. The
// frames notice, nothing is written, the hold goes, the level comes back and the editor is told the run ended.
BOOST_AUTO_TEST_CASE(AJobCancelledBeforeItStarted_EndsTheRunWithoutWriting)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	const auto before = snapshot(copy.root());
	std::atomic<bool> applied{ false };
	apply = [&](const pmg::migration_plan& plan, const pmg::apply_context& context) {
		applied = true;
		return pmg::apply_migration(plan, context);
	};
	// The pool's only io worker is busy, so the migration's job stays queued.
	std::atomic<bool> release{ false };
	auto blocker = job_tasks.run(ds::lane::io, ds::task_desc{ "busy io", "test", false }, [&release](ds::task_context&) {
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!release && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
	});
	static_cast<void>(ask());
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	run_frame(); // the modal
	run_frame(); // the checks, the hand-over
	BOOST_TEST(closed);
	BOOST_TEST(rs->changes_held());
	BOOST_TEST(jobs.cancel(job));
	run_frames();
	release = true;
	blocker.wait();
	settle_jobs();
	BOOST_TEST(!applied.load());
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST((running_said == std::vector<bool>{ true, false }));
	BOOST_TEST(attached);
	BOOST_TEST((snapshot(copy.root()) == before));
	const auto info = jobs.info(job);
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST((info->status == edt::job_status::cancelled));
}

// The project's own files: what a rollback must leave as it was (the journal and the backup under .snk/ stay).
std::map<std::string, std::string> project_files(const std::map<std::string, std::string>& files)
{
	std::map<std::string, std::string> out;
	for (const auto& [path, content] : files) {
		if (!path.starts_with(".snk")) out.emplace(path, content);
	}
	return out;
}

// A cancel before the commit point (job.cancel goes through the job's veto) is seen by the next checkpoint of the apply:
// the migration rolls back, the job ends cancelled, the editor gets its level back.
BOOST_AUTO_TEST_CASE(ACancelBeforeTheCommit_RollsTheMigrationBack)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	const auto before = project_files(snapshot(copy.root()));
	std::atomic<bool> entered{ false };
	std::atomic<bool> cancel_sent{ false };
	pmg::migration_status status = pmg::migration_status::in_progress;
	apply = [&](const pmg::migration_plan& plan, const pmg::apply_context& context) {
		entered = true;
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!cancel_sent && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		pmg::apply_outcome outcome = pmg::apply_migration(plan, context);
		status = outcome.report.status;
		return outcome;
	};
	static_cast<void>(ask());
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!entered && std::chrono::steady_clock::now() < deadline) {
		run_frame();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	BOOST_REQUIRE(entered.load());
	BOOST_TEST(!jobs.cancel_refusal(job).has_value());
	BOOST_TEST(jobs.cancel(job));
	cancel_sent = true;
	run_frames();
	settle_jobs();
	BOOST_TEST((status == pmg::migration_status::rolled_back));
	BOOST_TEST((project_files(snapshot(copy.root())) == before));
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST((running_said == std::vector<bool>{ true, false }));
	BOOST_TEST(attached);
	const auto info = jobs.info(job);
	BOOST_REQUIRE(info.has_value());
	BOOST_TEST((info->status == edt::job_status::cancelled));
}

// job_service::cancel_refusal is a question only: asking it during the run stops nothing.
BOOST_AUTO_TEST_CASE(AskingWhetherACancelWouldBeRefused_CancelsNothing)
{
	fs::remove(copy.res() / "broken.desc");
	std::atomic<bool> entered{ false };
	std::atomic<bool> asked{ false };
	apply = [&](const pmg::migration_plan& plan, const pmg::apply_context& context) {
		entered = true;
		const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!asked && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return pmg::apply_migration(plan, context);
	};
	static_cast<void>(ask());
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!entered && std::chrono::steady_clock::now() < deadline) {
		run_frame();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	BOOST_REQUIRE(entered.load());
	BOOST_TEST(!jobs.cancel_refusal(job).has_value());
	BOOST_TEST(!jobs.cancel_refusal(job).has_value());
	asked = true;
	run_frames();
	settle_jobs();
	const auto info = jobs.info(job);
	BOOST_REQUIRE(info.has_value());
	BOOST_REQUIRE((info->status == edt::job_status::done));
	const json::object& result = info->result->as_object();
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
	BOOST_TEST(!result.contains("cancel_ignored"));
}

// After the commit point a cancel is refused: the job ends with the migration's answer, which says the cancel came late.
BOOST_AUTO_TEST_CASE(ACancelAfterTheCommit_IsRefused_AndTheAnswerSaysSo)
{
	fs::remove(copy.res() / "broken.desc");
	std::atomic<bool> committed{ false };
	std::atomic<bool> cancel_tried{ false };
	apply = [&](const pmg::migration_plan& plan, const pmg::apply_context& context) {
		pmg::apply_context gated = context;
		gated.stop_requested = [&, inner = context.stop_requested](pmg::apply_checkpoint where) {
			const bool stop = inner && inner(where);
			if (where == pmg::apply_checkpoint::commit && !stop) {
				committed = true;
				const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(60);
				while (!cancel_tried && std::chrono::steady_clock::now() < limit) std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return stop;
		};
		return pmg::apply_migration(plan, gated);
	};
	static_cast<void>(ask());
	const cmd::result started = call("project.migrate");
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const auto job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!committed && std::chrono::steady_clock::now() < deadline) {
		run_frame();
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
	BOOST_REQUIRE(committed.load());
	BOOST_TEST(jobs.cancel_refusal(job).has_value());
	BOOST_TEST(!jobs.cancel(job));
	cancel_tried = true;
	run_frames();
	settle_jobs();
	const auto info = jobs.info(job);
	BOOST_REQUIRE(info.has_value());
	BOOST_REQUIRE((info->status == edt::job_status::done));
	BOOST_REQUIRE(info->result && info->result->is_object());
	const json::object& result = info->result->as_object();
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
	BOOST_TEST(result.at("cancel_ignored").as_bool());
	BOOST_TEST(std::string(result.at("text").as_string()).find("after the migration was committed") != std::string::npos);
}

// UI review of #761: only the plan that was shown is applied. A person's call (the window) passes the plan's fingerprint;
// a project that gives another plan by the run is refused with plan_changed and nothing is written.
BOOST_AUTO_TEST_CASE(APersonsRun_WithTheFingerprintOfAnOlderPlan_IsRefusedAsPlanChanged)
{
	fs::remove(copy.res() / "broken.desc");
	const cmd::result planned = call("project.migration_plan", {}, cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(planned.ok, planned.error);
	const std::string fingerprint(planned.value.at("fingerprint").as_string());
	BOOST_TEST(fingerprint.size() == 16u);
	// Someone edits a desc while the window shows the plan.
	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + "\n");
	const auto before = snapshot(copy.root());
	const json::object result = job_result(call("project.migrate", { { "plan_fingerprint", fingerprint } }, cmd::origin::ui()));
	BOOST_TEST(error_code_of(result) == "plan_changed", json::serialize(result));
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == before));
}

// For an agent the fingerprint only adds a check: one that is not the plan its ticket recorded is denied at the call.
BOOST_AUTO_TEST_CASE(AnAgentsForeignFingerprint_IsDenied_TheMatchingOneRuns)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	const cmd::result planned = call("project.migration_plan");
	BOOST_REQUIRE_MESSAGE(planned.ok, planned.error);
	const std::string fingerprint(planned.value.at("fingerprint").as_string());
	std::string foreign = fingerprint;
	foreign[0] = foreign[0] == '0' ? '1' : '0';

	static_cast<void>(ask());
	const cmd::result refused = call("project.migrate", { { "plan_fingerprint", foreign } });
	BOOST_TEST(refused.code == cmd::codes::denied);
	BOOST_TEST(call("project.migrate", { { "plan_fingerprint", "not a fingerprint" } }).code == cmd::codes::invalid_args);
	BOOST_TEST((snapshot(copy.root()) == before));
	BOOST_TEST(!closed);

	const json::object result = job_result(call("project.migrate", { { "plan_fingerprint", fingerprint } }));
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
}

// The game module swapped between the question and the run: the code that would migrate is not the one asked about.
BOOST_AUTO_TEST_CASE(ACodeChangeAfterTheQuestion_IsDenied)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	static_cast<void>(ask());
	++code_identity;
	const json::object result = migrate();
	BOOST_TEST(error_code_of(result) == cmd::codes::denied, json::serialize(result));
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == before));
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
	settle_jobs();
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
	settle_jobs();
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

// A link to /dev/zero and a FIFO in the project: the plan and the question are made without reading either (the test
// finishing is the check).
BOOST_AUTO_TEST_CASE(ALinkToADeviceAndAFifo_AreNotRead)
{
#ifdef _WIN32
	BOOST_TEST_MESSAGE("no /dev/zero or FIFO on Windows");
#else
	std::error_code link_error;
	fs::create_symlink("/dev/zero", copy.res() / "zero.desc", link_error);
	BOOST_REQUIRE_MESSAGE(!link_error, link_error.message());
	BOOST_REQUIRE(::mkfifo((copy.res() / "pipe.desc").c_str(), 0600) == 0);
	BOOST_TEST(!ask().empty());
	const cmd::result planned = call("project.migration_plan");
	BOOST_TEST(planned.ok);
	BOOST_TEST(planned.value.at("code").as_int64() == pmg::k_exit_refused);
#endif
}

// No byte changed, but the plan did (another step of a type after the question): the person approved another plan.
BOOST_AUTO_TEST_CASE(APlanThatChangedWithoutAByteChanged_IsDenied)
{
	fs::remove(copy.res() / "broken.desc");
	static_cast<void>(ask());
	const auto before = snapshot(copy.root());
	descs->register_migration("camera_desc", descs->current_version("camera_desc"), [](json::object&) {});
	const json::object result = migrate();
	BOOST_TEST(error_code_of(result) == cmd::codes::denied);
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == before));
}

// #761: the rollback of the editor is the command line's, run as a migration is: the files come back, the level is opened
// again, and the editor hears of every file the journal names.
BOOST_AUTO_TEST_CASE(ARollback_PutsTheProjectBack_AndOpensTheLevelAgain)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = content();
	current_level = res::tag::make("levels/old.desc");
	const std::string id = migrated();
	BOOST_TEST(!(content() == before));
	closed = false;
	attached = false;
	signaled.clear();
	running_said.clear();
	const json::object result = roll_back(id);
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_done);
	BOOST_TEST(std::string(result.at("status").as_string()) == "rolled_back");
	BOOST_TEST(std::string(result.at("id").as_string()) == id);
	BOOST_TEST(std::string(result.at("text").as_string()).find("rolled back") != std::string::npos);
	BOOST_TEST((content() == before));
	BOOST_TEST(closed);
	BOOST_TEST(attached);
	BOOST_TEST(result.at("level").at("reopened").as_bool());
	BOOST_TEST((std::ranges::find(signaled, std::string{ "res://cameras/cam_v1.desc" }) != signaled.end()));
	BOOST_TEST((rolled_back_ids == std::vector<std::string>{ id }));
	BOOST_TEST((running_said == std::vector<bool>{ true, false }));
	BOOST_TEST(!rs->changes_held());
}

// A file changed after the migration refuses the whole rollback: code 2, nothing written, the level is opened again.
BOOST_AUTO_TEST_CASE(ARefusedRollback_IsCode2_AndWritesNothing)
{
	current_level = res::tag::make("levels/old.desc");
	const std::string id = migrated();
	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	signaled.clear();
	const json::object result = roll_back(id);
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_refused, json::serialize(result));
	BOOST_TEST(result.contains("refused"));
	BOOST_TEST((snapshot(copy.root()) == changed));
	BOOST_TEST(signaled.empty());
	BOOST_TEST(result.at("level").at("reopened").as_bool());
	BOOST_TEST(rolled_back_ids.empty());

	// No journal of that name: refused before anything is closed.
	closed = false;
	const json::object unknown = roll_back("20990101T000000Z-0.0.0");
	BOOST_TEST(unknown.at("code").as_int64() == pmg::k_exit_refused);
	BOOST_TEST(!closed);
}

BOOST_AUTO_TEST_CASE(TheRollback_TakesOnlyAMigrationId)
{
	for (const json::object& args : { json::object{}, json::object{ { "id", "" } }, json::object{ { "id", 5 } },
			 json::object{ { "id", "x" }, { "force", true } } }) {
		BOOST_TEST_CONTEXT(json::serialize(args)) {
			BOOST_TEST(call("project.migrate_rollback", args, cmd::origin::ui()).code == cmd::codes::invalid_args);
		}
	}
	for (const char* id : { "..", ".", ".hidden", "../x", "a/b", "a\\b", "c:x", "a b", "a#b" }) {
		BOOST_TEST_CONTEXT(id) {
			BOOST_TEST(call("project.migrate_rollback", { { "id", id } }, cmd::origin::ui()).code == cmd::codes::invalid_args);
		}
	}
	const cmd::command_desc* rollback_command = commands.find("project.migrate_rollback");
	BOOST_REQUIRE(rollback_command);
	BOOST_TEST((rollback_command->category == cmd::agent_category::project_migrate));
	BOOST_TEST((rollback_command->risk == cmd::danger::confirm));
	BOOST_TEST(rollback_command->mutates);
	BOOST_TEST(rollback_command->starts_job);
	BOOST_TEST(rollback_command->describe_before_allowed_invoke);
}

// An agent's rollback runs only after the question, and only while the journal is the one the person was shown.
BOOST_AUTO_TEST_CASE(AnAgentsRollback_IsBoundToTheJournalItWasShown)
{
	const std::string id = migrated();
	const auto migrated_content = content();
	BOOST_TEST(call("project.migrate_rollback", { { "id", id } }).code == cmd::codes::denied);
	const std::string text = ask_rollback(id);
	BOOST_TEST(text.find(id) != std::string::npos, text);
	BOOST_TEST(text.find("cam_v1.desc") != std::string::npos, text);
	const fs::path journal = copy.root() / ".snk" / "migrations" / id / "report.json";
	write_text(journal, read_text(journal) + "
");
	const json::object changed = roll_back(id, cmd::origin::agent("s1"));
	BOOST_TEST(error_code_of(changed) == cmd::codes::denied, json::serialize(changed));
	BOOST_TEST((content() == migrated_content));

	static_cast<void>(ask_rollback(id));
	const json::object result = roll_back(id, cmd::origin::agent("s1"));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_done, json::serialize(result));
	// A question about a rollback leaves the caller's plan alone, and the other way round.
	BOOST_TEST(ask_rollback("x").find("will be refused") != std::string::npos);
}

// A rollback that stops on its way is code 6: the level stays detached and unsaved until that migration is rolled back.
// An exception before the rollback wrote anything (its journal still says the migration ended): nothing changed, so it is a
// refusal and the level is opened again; no save block is left behind on a detached level (#761 review).
BOOST_AUTO_TEST_CASE(ARollbackThatThrowsBeforeItWrote_IsARefusal_AndTheLevelComesBack)
{
	current_level = res::tag::make("levels/old.desc");
	const std::string id = migrated();
	const auto before = snapshot(copy.root());
	attached = false;
	rollback = [](const fs::path&, std::string_view, const pmg::rollback_context&) -> std::optional<std::string> {
		throw std::runtime_error("injected rollback failure");
	};
	const json::object result = roll_back(id);
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_refused, json::serialize(result));
	BOOST_TEST(std::string(result.at("refused").as_string()).find("injected rollback failure") != std::string::npos);
	BOOST_TEST(attached);
	BOOST_TEST(result.at("level").at("reopened").as_bool());
	BOOST_TEST(!left_detached.has_value());
	BOOST_TEST((snapshot(copy.root()) == before));
}

// An exception after the journal says in_progress: code 6, the level stays detached and its save block stays until that
// migration is rolled back.
BOOST_AUTO_TEST_CASE(ARollbackThatThrowsAfterItBegan_IsCode6_AndTheLevelStaysDetached)
{
	current_level = res::tag::make("levels/old.desc");
	const std::string id = migrated();
	attached = false;
	rollback = [](const fs::path& root, std::string_view journal_id, const pmg::rollback_context& context) -> std::optional<std::string> {
		write_text(root / ".snk/migrations" / std::string(journal_id) / "report.json", R"({"status":"in_progress"})");
		if (context.wrote) *context.wrote = true;
		throw std::runtime_error("injected rollback failure");
	};
	const json::object result = roll_back(id);
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_rollback_failed, json::serialize(result));
	BOOST_TEST(std::string(result.at("error").as_string()).find("injected rollback failure") != std::string::npos);
	BOOST_TEST(!attached);
	BOOST_TEST(!result.at("level").at("reopened").as_bool());
	BOOST_REQUIRE(left_detached.has_value());
	BOOST_TEST(left_detached->find("--migrate-rollback " + id) != std::string::npos, *left_detached);
	BOOST_REQUIRE(left_detached_rolled_back);
	BOOST_TEST(!left_detached_rolled_back());
	BOOST_TEST(rolled_back_ids.empty());
}

// rollback_migration itself fails half way (a restore that cannot be written): code 6 from the journal's rollback_failed,
// the level stays detached; a retry refused before it writes is code 2 (nothing written), not 6.
BOOST_AUTO_TEST_CASE(ARealRollbackFailure_IsCode6_ARefusedRetryIsCode2)
{
	current_level = res::tag::make("levels/old.desc");
	const std::string id = migrated();
	attached = false;
	rollback = [](const fs::path& root, std::string_view journal_id, const pmg::rollback_context& context) {
		pmg::rollback_context failing = context;
		failing.write = [](const fs::path& at, const fs::path& relative, std::string_view text) -> std::optional<std::string> {
			if (!relative.generic_string().starts_with(".snk/")) return std::string{ "injected restore failure" };
			return prj::write_project_file(at, relative, text);
		};
		return pmg::rollback_migration(root, journal_id, failing);
	};
	const json::object failed = roll_back(id);
	BOOST_TEST(failed.at("code").as_int64() == pmg::k_exit_rollback_failed, json::serialize(failed));
	BOOST_TEST(std::string(failed.at("status").as_string()) == "rollback_failed");
	BOOST_TEST(std::string(failed.at("error").as_string()).find("injected restore failure") != std::string::npos);
	BOOST_TEST(!attached);
	BOOST_REQUIRE(left_detached_rolled_back);
	BOOST_TEST(!left_detached_rolled_back());

	// A file changed since: the retry refuses before its first write.
	rollback = {};
	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	const json::object retry = roll_back(id);
	BOOST_TEST(retry.at("code").as_int64() == pmg::k_exit_refused, json::serialize(retry));
	BOOST_TEST((snapshot(copy.root()) == changed));
}

// The editor's wiring through the hooks: a rollback of the migration that stopped lifts its save block and attaches the
// level again.
BOOST_AUTO_TEST_CASE(ARollbackOfAStoppedMigration_LiftsItsBlock_AndAttachesTheLevel)
{
	fs::remove(copy.res() / "broken.desc");
	current_level = res::tag::make("levels/old.desc");
	apply = [](const pmg::migration_plan&, const pmg::apply_context& context) -> pmg::apply_outcome {
		write_text(context.root / ".snk/migrations/test-stopped/report.json", R"({"status":"in_progress"})");
		throw std::runtime_error("injected apply failure");
	};
	const json::object stopped = migrate(cmd::origin::ui());
	BOOST_REQUIRE(stopped.at("code").as_int64() == pmg::k_exit_rollback_failed);
	BOOST_REQUIRE(left_detached.has_value());
	BOOST_TEST(!attached);

	rollback = [](const fs::path& root, std::string_view journal_id, const pmg::rollback_context& context) -> std::optional<std::string> {
		if (context.wrote) *context.wrote = true;
		write_text(root / ".snk/migrations" / std::string(journal_id) / "report.json", R"({"status":"rolled_back"})");
		return std::nullopt;
	};
	const json::object result = roll_back("test-stopped");
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_done, json::serialize(result));
	BOOST_TEST((rolled_back_ids == std::vector<std::string>{ "test-stopped" }));
	BOOST_TEST(!left_detached.has_value());
	BOOST_TEST(attached);
	BOOST_TEST(result.at("level").at("reopened").as_bool());
}

// A cancel after the rollback's commit point is refused; the answer says so.
BOOST_AUTO_TEST_CASE(ACancelAfterTheRollbackBegan_IsRefused_AndTheAnswerSaysSo)
{
	const std::string id = migrated();
	std::atomic<bool> entered{ false };
	std::atomic<bool> release{ false };
	rollback = [&entered, &release](const fs::path&, std::string_view, const pmg::rollback_context&) -> std::optional<std::string> {
		entered = true;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		while (!release && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return std::nullopt;
	};
	const cmd::result started = call("project.migrate_rollback", { { "id", id } }, cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	const std::uint64_t job = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (!entered && std::chrono::steady_clock::now() < deadline) {
		run_frame();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	BOOST_REQUIRE_MESSAGE(entered.load(), "the rollback did not begin within 60 s");
	BOOST_TEST(jobs.cancel_refusal(job).has_value());
	BOOST_TEST(!jobs.cancel(job));
	release = true;
	run_frames();
	settle_jobs();
	const auto info = jobs.info(job);
	BOOST_REQUIRE(info.has_value());
	BOOST_REQUIRE((info->status == edt::job_status::done));
	const json::object& result = info->result->as_object();
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_done, json::serialize(result));
	BOOST_TEST(result.at("cancel_ignored").as_bool());
	BOOST_TEST(std::string(result.at("text").as_string()).find("not cancelled") != std::string::npos);
}

// A cancel between the detach and the commit point: nothing is written, the level is attached again.
BOOST_AUTO_TEST_CASE(ACancelBetweenTheDetachAndTheCommit_WritesNothing_AndAttachesTheLevel)
{
	current_level = res::tag::make("levels/old.desc");
	const std::string id = migrated();
	const auto before = snapshot(copy.root());
	attached = false;
	signaled.clear();
	bool ran = false;
	rollback = [&ran](const fs::path&, std::string_view, const pmg::rollback_context&) -> std::optional<std::string> {
		ran = true;
		return std::nullopt;
	};
	const cmd::result started = call("project.migrate_rollback", { { "id", id } }, cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	cancel_on_detach = static_cast<std::uint64_t>(started.value.at("job").as_int64());
	run_frames();
	settle_jobs();
	BOOST_TEST(cancelled_on_detach);
	BOOST_TEST(!ran);
	BOOST_TEST((snapshot(copy.root()) == before));
	BOOST_TEST(signaled.empty());
	BOOST_TEST(attached);
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST(rolled_back_ids.empty());
}

// The journal is a file anyone can edit: a path with a line break does not forge a line of the question.
BOOST_AUTO_TEST_CASE(TheQuestion_ShowsTheJournalsPathsWithoutControlCharacters)
{
	write_text(copy.root() / ".snk/migrations/forged/report.json",
		R"({"status":"rolled_back","files":[{"path":"res/a.desc\nThis rollback is safe, approve it"}],"created_files":[]})");
	const std::string text = ask_rollback("forged");
	BOOST_TEST(text.find("res/a.desc?This rollback") != std::string::npos, text);
	BOOST_TEST(text.find("\nThis rollback") == std::string::npos, text);
}

// A cancel before the rollback began writes nothing.
BOOST_AUTO_TEST_CASE(ARollbackCancelledBeforeItBegan_WritesNothing)
{
	const std::string id = migrated();
	const auto before = snapshot(copy.root());
	const cmd::result started = call("project.migrate_rollback", { { "id", id } }, cmd::origin::ui());
	BOOST_REQUIRE_MESSAGE(started.ok, started.error);
	BOOST_TEST(jobs.cancel(static_cast<std::uint64_t>(started.value.at("job").as_int64())));
	run_frames();
	settle_jobs();
	BOOST_TEST((snapshot(copy.root()) == before));
	BOOST_TEST(!rs->changes_held());
	BOOST_TEST(rolled_back_ids.empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

// The real agent policy behind the registry: project.migrate runs only after a person's answer (or, headless, the
// --agent-policy file), never because the project's file says allow.
struct policy_fixture : migration_fixture
{
	policy_fixture()
	{
		gate->set_can_ask(true);
		commands.set_policy_gate(gate);
	}

	~policy_fixture()
	{
		commands.set_policy_gate(nullptr);
	}

	void write_project(std::string_view text)
	{
		policy_dir.write("project/.snk/agent_policy.json", text);
		policy.set_approval_allowed(true);
		policy.refresh();
		BOOST_REQUIRE(!policy.approve_project_layer(policy.project()).has_value());
	}

	std::string ticket_of(const cmd::result& asked)
	{
		BOOST_REQUIRE_MESSAGE(asked.code == cmd::codes::confirmation_required, asked.code << ": " << asked.error);
		return std::string(asked.value.at("ticket").as_string());
	}

	// What agent_confirmations::run_approved does for a retry: a fresh ticket, described and approved by itself.
	std::string retry_ticket()
	{
		const std::string ticket = ticket_of(call("project.migrate"));
		BOOST_REQUIRE(commands.approve(ticket));
		internal_tickets.insert(ticket);
		return ticket;
	}

	cmd::result call_with(const std::string& ticket)
	{
		cmd::call request;
		request.args = json::object{};
		request.from = cmd::origin::agent("s1");
		request.confirmation = ticket;
		return commands.invoke("project.migrate", request);
	}

	test_util::temp_dir policy_dir{ "edt_migration_commands_policy" };
	edt::agent_policy policy{ policy_dir.path() / "project" / ".snk" / "agent_policy.json", policy_dir.path() / "personal.json" };
	std::shared_ptr<edt::agent_policy_gate> gate = std::make_shared<edt::agent_policy_gate>(policy);
};

}

BOOST_FIXTURE_TEST_SUITE(EdtMigrationCommandsPolicy, policy_fixture)

BOOST_AUTO_TEST_CASE(TheProjectsFile_CannotAllowTheRollback_ItStillAsks)
{
	for (const char* text : { R"({"categories":{"project_migrate":"allow"}})", R"({"commands":{"project.migrate_rollback":"allow"}})" }) {
		BOOST_TEST_CONTEXT(text) {
			write_project(text);
			BOOST_TEST((policy.effective(*commands.find("project.migrate_rollback")).mode == cmd::policy_mode::ask));
			BOOST_TEST(call("project.migrate_rollback", { { "id", "x" } }).code == cmd::codes::confirmation_required);
			BOOST_TEST(implicit.empty());
		}
	}
}

BOOST_AUTO_TEST_CASE(TheProjectsFile_CannotAllowTheMigration_ItStillAsks)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	for (const char* text : { R"({"categories":{"project_migrate":"allow"}})", R"({"commands":{"project.migrate":"allow"}})" }) {
		BOOST_TEST_CONTEXT(text) {
			write_project(text);
			BOOST_TEST((policy.effective(*commands.find("project.migrate")).mode == cmd::policy_mode::ask));
			const cmd::result asked = call("project.migrate");
			BOOST_TEST(asked.code == cmd::codes::confirmation_required);
			BOOST_TEST(implicit.empty());
		}
	}
	BOOST_TEST((snapshot(copy.root()) == before));
}

// The person says no: nothing runs, the ticket is spent, and the next call is a new question.
BOOST_AUTO_TEST_CASE(ADeclinedQuestion_RunsNothing_AndIsNotReused)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	const std::string declined = ticket_of(call("project.migrate"));
	commands.discard(declined);
	BOOST_TEST(call_with(declined).code == cmd::codes::denied);
	BOOST_TEST(implicit.empty());
	BOOST_TEST((snapshot(copy.root()) == before));

	const std::string asked = ticket_of(call("project.migrate"));
	BOOST_TEST(asked != declined);
	BOOST_REQUIRE(commands.approve(asked));
	const json::object result = job_result(call_with(asked));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
	// The plan was taken by that run: another run needs another question.
	BOOST_TEST(call_with(asked).code == cmd::codes::denied);
}

// What agent_confirmations does after a person's yes that met `busy`: a retry with a fresh ticket, whose question sees the
// project as it is then. The plan the person approved still judges it: a project changed meanwhile is denied.
BOOST_AUTO_TEST_CASE(ARetryAfterBusy_IsJudgedByThePlanThePersonApproved)
{
	fs::remove(copy.res() / "broken.desc");
	const std::string approved = ticket_of(call("project.migrate"));
	BOOST_REQUIRE(commands.approve(approved));
	busy = "Play is running";
	BOOST_TEST(call_with(approved).code == cmd::codes::busy);
	BOOST_TEST(implicit.empty());

	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	busy.reset();
	const json::object result = job_result(call_with(retry_ticket()));
	BOOST_TEST(error_code_of(result) == cmd::codes::denied);
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == changed));
}

// Approved at the end of the question's life: the approved plan lives on from the refusal, so the retry after the
// question's lifetime is still judged by it, not by the plan described for the retry.
BOOST_AUTO_TEST_CASE(AnApprovedPlan_OutlivesItsQuestion_AndJudgesTheRetry)
{
	fs::remove(copy.res() / "broken.desc");
	const std::string approved = ticket_of(call("project.migrate"));
	clock += std::chrono::seconds(117);
	BOOST_REQUIRE(commands.approve(approved));
	busy = "Play is running";
	BOOST_TEST(call_with(approved).code == cmd::codes::busy);

	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	clock += std::chrono::seconds(4);
	busy.reset();
	const json::object result = job_result(call_with(retry_ticket()));
	BOOST_TEST(error_code_of(result) == cmd::codes::denied);
	BOOST_TEST(!closed);
	BOOST_TEST((snapshot(copy.root()) == changed));
}

// The same with the project unchanged: the retry migrates, so the approved plan was kept past the question's lifetime.
BOOST_AUTO_TEST_CASE(AnApprovedPlan_OutlivesItsQuestion_AndRunsTheRetry)
{
	fs::remove(copy.res() / "broken.desc");
	const std::string approved = ticket_of(call("project.migrate"));
	clock += std::chrono::seconds(117);
	BOOST_REQUIRE(commands.approve(approved));
	busy = "Play is running";
	BOOST_TEST(call_with(approved).code == cmd::codes::busy);

	clock += std::chrono::seconds(4);
	busy.reset();
	const json::object result = job_result(call_with(retry_ticket()));
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
}

// A retry whose approved plan is gone is denied; it never falls back to the plan described for its own ticket.
BOOST_AUTO_TEST_CASE(ARetryWithoutTheApprovedPlan_IsDenied)
{
	fs::remove(copy.res() / "broken.desc");
	const auto before = snapshot(copy.root());
	const std::string approved = ticket_of(call("project.migrate"));
	BOOST_REQUIRE(commands.approve(approved));
	busy = "Play is running";
	BOOST_TEST(call_with(approved).code == cmd::codes::busy);

	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	clock += std::chrono::seconds(300);
	busy.reset();
	const cmd::result retried = call_with(retry_ticket());
	BOOST_TEST(retried.code == cmd::codes::denied, retried.error);
	BOOST_TEST(implicit.empty());
	BOOST_TEST((snapshot(copy.root()) == changed));
	BOOST_TEST((snapshot(copy.root()) != before));
}

// Questions of other sessions fill the records: the approved plan stays, and still judges the retry.
BOOST_AUTO_TEST_CASE(QuestionsOfOtherSessions_DoNotEvictTheApprovedPlan)
{
	fs::remove(copy.res() / "broken.desc");
	const std::string approved = ticket_of(call("project.migrate"));
	BOOST_REQUIRE(commands.approve(approved));
	busy = "Play is running";
	BOOST_TEST(call_with(approved).code == cmd::codes::busy);
	for (int i = 0; i < 40; ++i) static_cast<void>(ask(cmd::origin::agent(std::format("other-{}", i))));

	write_text(copy.res() / "cameras/cam_v1.desc", read_text(copy.res() / "cameras/cam_v1.desc") + " ");
	const auto changed = snapshot(copy.root());
	busy.reset();
	const json::object result = job_result(call_with(retry_ticket()));
	BOOST_TEST(error_code_of(result) == cmd::codes::denied);
	BOOST_TEST((snapshot(copy.root()) == changed));
}

// The same with the project unchanged: the retry migrates, so the approved plan was still there.
BOOST_AUTO_TEST_CASE(QuestionsOfOtherSessions_LeaveTheApprovedPlanToRun)
{
	fs::remove(copy.res() / "broken.desc");
	const std::string approved = ticket_of(call("project.migrate"));
	BOOST_REQUIRE(commands.approve(approved));
	busy = "Play is running";
	BOOST_TEST(call_with(approved).code == cmd::codes::busy);
	for (int i = 0; i < 40; ++i) static_cast<void>(ask(cmd::origin::agent(std::format("other-{}", i))));

	busy.reset();
	const json::object result = job_result(call_with(retry_ticket()));
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);
}

// Headless, the --agent-policy file answers the question in advance; the plan is still described and checked.
BOOST_AUTO_TEST_CASE(Headless_TheAgentPolicyFile_RunsIt_WithThePlanDescribed)
{
	fs::remove(copy.res() / "broken.desc");
	// The project is its own worktree: the answers lie outside it.
	fs::create_directories(copy.root() / ".git");
	gate->set_can_ask(false);
	policy.set_headless(true);
	policy_dir.write("answers.json", R"({"commands":{"project.migrate":"allow"}})");
	policy.set_preanswers_file(policy_dir.path() / "answers.json", copy.root());
	BOOST_REQUIRE(policy.preanswers_error().empty());
	const json::object result = migrate();
	BOOST_TEST_REQUIRE(error_code_of(result).empty(), json::serialize(result));
	BOOST_TEST(result.at("code").as_int64() == pmg::k_exit_partial);

	// Without the answer, headless nobody is asked: denied.
	policy_dir.write("none.json", R"({})");
	policy.set_preanswers_file(policy_dir.path() / "none.json", copy.root());
	BOOST_TEST(call("project.migrate").code == cmd::codes::denied);
}

BOOST_AUTO_TEST_SUITE_END()
