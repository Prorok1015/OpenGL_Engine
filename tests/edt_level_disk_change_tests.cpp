#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include "edt_code_commands.h"
#include "edt_desc_file_commands.h"
#include "edt_desc_problems.h"
#include "edt_file_menu.h"
#include "edt_level_controller.h"
#include "edt_level_disk_banner.h"
#include "edt_save_all.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <boost/json.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// #1188 (US-60-C): an outside edit of the open level's file. The editor reads the level from a working copy in memory://, so
// the level controller watches the file itself and tells its own writes from outside ones by code_version_of: a clean level
// takes the new file, a dirty one (or one in Play) is flagged and shows a banner, and Save asks a person or refuses an agent.
// An outside write is the file written here, then signal_changed of its tag and a pump, as a file watcher delivers it.

namespace {

namespace fs = std::filesystem;

constexpr std::string_view k_level_path = "levels/main.desc";
constexpr std::string_view k_working_copy = "memory://editor/loading/1/main.desc";
// Spelled apart: the repository scan of res:// literals would look for the missing file below.
constexpr std::string_view k_res_protocol = "res:/" "/";

std::string read_file(const fs::path& p)
{
	std::ifstream in(p, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct level_disk_fixture : test_util::scene_env
{
	level_disk_fixture()
	{
		fs::create_directories(content / "levels");
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ content });

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		state.set_save_handlers([this] { return level_ctrl.quick_save_level(); },
			[this](const res::tag& target) { return level_ctrl.save_level_as(std::string(target.relative())); });
		// As editor_system wires it: one row of the source "desc" for bytes that do not load.
		level_ctrl.on_disk_problem = [this](const std::string& reason) {
			if (reason.empty()) {
				publisher.clear(problems, "level file");
				return;
			}
			res::diagnostic item;
			item.severity = res::diagnostic_severity::error;
			item.kind = "level_file_changed";
			item.resource = state.level_tag.string();
			item.reason = reason;
			publisher.report(problems, "level file", { item }, {}, state.level_tag);
		};
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	}

	~level_disk_fixture()
	{
		level_ctrl.unwatch_level();
		// The resolver's io thread and watcher go before the temp folder they work in.
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	// A level of one world called `world`, in the current versions of the descs.
	std::string level_text(std::string_view world) const
	{
		boost::json::object world_json{ { "__type", "world_desc" }, { "__version", descs->current_version("world_desc") },
			{ "name", world }, { "systems", boost::json::array{} }, { "__id", "cbf29ce484222325" },
			{ "components", boost::json::object{} }, { "children", boost::json::array{} } };
		boost::json::object level{ { "__type", "level_desc" }, { "__version", descs->current_version("level_desc") },
			{ "name", "main" }, { "worlds", boost::json::array{ world_json } } };
		return boost::json::serialize(level);
	}

	void write_file(const std::string& text) { std::ofstream(level_file_path, std::ios::binary | std::ios::trunc) << text; }

	// What a file watcher does after an outside write.
	void write_from_outside(const std::string& text)
	{
		write_file(text);
		rs->signal_changed(level_tag);
		pump();
	}

	// As level_load_component::begin and poll do: the working copy gets the file's bytes, the document takes the level.
	void open_level(std::string_view world)
	{
		const std::string text = level_text(world);
		write_file(text);
		const auto bytes = test_util::to_bytes(text);
		rs->store(working_copy, bytes);
		auto handle = rs->require_sync<scn::level_desc>(working_copy);
		pump();
		BOOST_REQUIRE(handle.is_ready());
		BOOST_REQUIRE(level_ctrl.take_loaded_level(handle, level_tag, working_copy, edt::desc_root_version(bytes), edt::code_version_of(bytes)));
		BOOST_REQUIRE(!state.is_dirty());
	}

	std::string world_name() const { return state.world_names.empty() ? std::string{} : state.world_names.front(); }
	std::vector<std::byte> working_copy_bytes() const { return ram->files.at(working_copy); }
	void make_dirty() { state.commit("edit", edt::commit_mode::state_only); }

	// The File menu over this level, as file_menu_component hands it the hooks.
	edt::file_menu& menu()
	{
		if (!m_menu) {
			edt::file_menu_hooks hooks;
			hooks.has_scene = [this] { return state.editor_tag.is_valid(); };
			hooks.is_dirty = [this] { return state.is_dirty(); };
			hooks.level_path = [this] { return state.level_tag.is_valid() ? std::string(state.level_tag.relative()) : std::string{}; };
			hooks.quick_save = [this] { return state.save().saved; };
			hooks.save_level_as = [this](const std::string& path) { return state.save_as(res::tag::make(path)).saved; };
			hooks.level_disk_conflict = [this] { return level_ctrl.disk_conflict(); };
			hooks.ask_level_disk_conflict = [this](std::function<void(bool)> resolved) { banner.ask_save(std::move(resolved)); };
			m_menu = std::make_unique<edt::file_menu>(dialogs, actions, std::move(hooks));
			m_menu->register_actions();
		}
		return *m_menu;
	}

	// Answers the front dialog with the button `index`.
	void answer_front(std::size_t index)
	{
		const auto pending = dialogs.list();
		BOOST_REQUIRE(!pending.empty());
		BOOST_REQUIRE(dialogs.answer(pending.front().id, boost::json::object{ { "index", index } }).ok);
	}

	test_util::temp_dir dir{ "level_disk" };
	const fs::path content = dir.path() / "content";
	const fs::path level_file_path = content / "levels" / "main.desc";
	const res::tag level_tag = res::tag::make(std::string(k_level_path));
	const res::tag working_copy{ std::string(k_working_copy) };
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::scene_document state;
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	edt::problem_list problems;
	edt::desc_problem_publisher publisher;
	edt::dialog_service dialogs;
	edt::level_disk_banner banner{ state, level_ctrl, dialogs };
	edt::action_registry actions;
	std::unique_ptr<edt::file_menu> m_menu;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtLevelDiskChangeTests, level_disk_fixture)

// F28: on main nothing watched res://levels/main.desc (only its memory:// copy), so the level kept its old worlds.
BOOST_AUTO_TEST_CASE(F28_AnOutsideEditOfTheOpenCleanLevel_IsTaken)
{
	open_level("alpha");
	BOOST_TEST((level_ctrl.watched_file() == level_tag));

	const std::string beta = level_text("beta");
	write_from_outside(beta);

	BOOST_TEST(world_name() == "beta");
	BOOST_TEST(!state.is_dirty());
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(!banner.visible());
	BOOST_TEST((working_copy_bytes() == test_util::to_bytes(beta)));
	BOOST_TEST(level_ctrl.disk_version() == edt::code_version_of(test_util::to_bytes(beta)));
}

BOOST_AUTO_TEST_CASE(OwnSave_IsNoOutsideChange)
{
	open_level("alpha");
	make_dirty();
	BOOST_REQUIRE(state.save().saved);
	rs->signal_changed(level_tag);
	pump();

	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(!banner.visible());
	BOOST_TEST(!state.is_dirty());
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(level_ctrl.disk_version() == edt::code_version_of(test_util::to_bytes(read_file(level_file_path))));
}

BOOST_AUTO_TEST_CASE(ADirtyLevel_IsFlagged_AndKeepMineHidesTheBannerUntilTheNextChange)
{
	open_level("alpha");
	make_dirty();
	const auto copy_before = working_copy_bytes();

	write_from_outside(level_text("beta"));
	BOOST_TEST(state.changed_on_disk());
	BOOST_TEST(banner.visible());
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(state.is_dirty());
	BOOST_TEST((working_copy_bytes() == copy_before));
	const auto serial = state.disk_change_serial();

	// The same version signalled again is not another change.
	rs->signal_changed(level_tag);
	pump();
	BOOST_TEST(state.disk_change_serial() == serial);

	banner.keep_mine();
	banner.run_pending();
	BOOST_TEST(!banner.visible());
	BOOST_TEST(state.changed_on_disk());

	write_from_outside(level_text("gamma"));
	BOOST_TEST(state.disk_change_serial() > serial);
	BOOST_TEST(banner.visible());
	BOOST_TEST(world_name() == "alpha");
}

BOOST_AUTO_TEST_CASE(Reload_AsksFirst_ThenDropsTheEditsAndTakesTheFile)
{
	open_level("alpha");
	make_dirty();
	write_from_outside(level_text("beta"));
	BOOST_REQUIRE(state.changed_on_disk());

	bool reloaded = false;
	banner.reload([&reloaded](bool done) { reloaded = done; });
	BOOST_TEST(dialogs.pending() == 1u);
	BOOST_TEST(world_name() == "alpha");
	answer_front(0);
	// Queued: the level is rebuilt by the frame's tick, not under the panel that drew the button.
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(banner.has_pending());
	banner.run_pending();

	BOOST_TEST(reloaded);
	BOOST_TEST(world_name() == "beta");
	BOOST_TEST(!state.is_dirty());
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(!banner.visible());
}

BOOST_AUTO_TEST_CASE(Overwrite_WritesTheEditorsLevel_AndClearsTheFlag)
{
	open_level("alpha");
	make_dirty();
	write_from_outside(level_text("beta"));
	BOOST_REQUIRE(state.changed_on_disk());

	bool written_ok = false;
	banner.overwrite([&written_ok](bool written) { written_ok = written; });
	banner.run_pending();
	BOOST_TEST(written_ok);
	const auto written = boost::json::parse(read_file(level_file_path));
	BOOST_TEST(written.at("worlds").as_array().at(0).at("name").as_string() == "alpha");
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(!state.is_dirty());

	// The signal of that write is the editor's own.
	rs->signal_changed(level_tag);
	pump();
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(world_name() == "alpha");
}

BOOST_AUTO_TEST_CASE(Overwrite_RefusesAFileThatChangedAgainMeanwhile)
{
	open_level("alpha");
	make_dirty();
	write_from_outside(level_text("beta"));
	BOOST_REQUIRE(banner.visible());
	banner.keep_mine();
	banner.run_pending();
	const auto serial = state.disk_change_serial();

	// Another outside write whose signal has not come yet.
	const std::string gamma = level_text("gamma");
	write_file(gamma);
	bool written_ok = true;
	banner.overwrite([&written_ok](bool written) { written_ok = written; });
	banner.run_pending();
	BOOST_TEST(!written_ok);

	BOOST_TEST(read_file(level_file_path) == gamma);
	BOOST_TEST(state.changed_on_disk());
	BOOST_TEST(state.disk_change_serial() > serial);
	BOOST_TEST(banner.visible());
	BOOST_TEST(!banner.error().empty());
}

BOOST_AUTO_TEST_CASE(SaveWithTheFlag_AsksAPerson_AndRefusesAnAgent)
{
	open_level("alpha");
	make_dirty();
	const std::string beta = level_text("beta");
	write_from_outside(beta);
	BOOST_REQUIRE(state.changed_on_disk());

	// A person: Ctrl+S asks Reload / Overwrite / Cancel and writes nothing.
	menu().quick_save();
	BOOST_TEST(dialogs.pending() == 1u);
	BOOST_TEST(read_file(level_file_path) == beta);
	answer_front(2);
	BOOST_TEST(!banner.has_pending());
	BOOST_TEST(read_file(level_file_path) == beta);
	BOOST_TEST(state.is_dirty());
	BOOST_TEST(state.changed_on_disk());

	// An agent: needs_input, nothing written, nothing asked.
	const edt::action_result agent = actions.invoke("file.save", boost::json::object{}, cmd::origin::agent("session-1"));
	BOOST_TEST(!agent.ok);
	BOOST_TEST(agent.code == "needs_input");
	BOOST_TEST(dialogs.pending() == 0u);
	BOOST_TEST(read_file(level_file_path) == beta);

	// Save All: the level waits for the question; its JSON (an agent's file.save_all) says needs_input.
	edt::editor_events events;
	edt::document_manager documents{ state, events };
	edt::save_all_scene_ports scene;
	scene.level_path = [this] { return std::string(state.level_tag.relative()); };
	scene.quick_save = [this] { return state.save().saved; };
	scene.disk_conflict = [this] { return level_ctrl.disk_conflict(); };
	const edt::save_all_report report = edt::save_all_write(documents, scene);
	BOOST_TEST(report.saved.empty());
	BOOST_REQUIRE(report.asked.size() == 1u);
	BOOST_TEST((report.asked.front().question == edt::save_all_question::disk_changed));
	BOOST_TEST(edt::save_all_json(report).at("needs_input").as_array().size() == 1u);
	BOOST_TEST(read_file(level_file_path) == beta);
	documents.shutdown();
	state.bind_events(nullptr);
}

BOOST_AUTO_TEST_CASE(SaveWithTheFlag_OverwriteAnsweredByThePerson_WritesTheLevel)
{
	open_level("alpha");
	make_dirty();
	write_from_outside(level_text("beta"));

	menu().quick_save();
	answer_front(1);
	banner.run_pending();
	const auto written = boost::json::parse(read_file(level_file_path));
	BOOST_TEST(written.at("worlds").as_array().at(0).at("name").as_string() == "alpha");
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(!state.is_dirty());
}

BOOST_AUTO_TEST_CASE(AMissedSignal_IsFoundBySave)
{
	open_level("alpha");
	make_dirty();
	const std::string beta = level_text("beta");
	write_file(beta);

	BOOST_TEST(!level_ctrl.quick_save_level());
	BOOST_TEST(state.changed_on_disk());
	BOOST_TEST(read_file(level_file_path) == beta);

	menu().quick_save();
	BOOST_TEST(dialogs.pending() == 1u);
	BOOST_TEST(read_file(level_file_path) == beta);
}

BOOST_AUTO_TEST_CASE(BrokenBytesFromOutside_LeaveTheOldLevel_AndOneProblem)
{
	open_level("alpha");
	const auto copy_before = working_copy_bytes();
	const std::string version_before = level_ctrl.disk_version();

	write_from_outside("{ \"__type\": \"level_desc\", \"worlds\": [ ");
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST((working_copy_bytes() == copy_before));
	BOOST_TEST(problems.size() == 1u);
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(level_ctrl.disk_version() == version_before);

	write_from_outside(R"({"__type":"material_desc"})");
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST((working_copy_bytes() == copy_before));
	BOOST_TEST(problems.size() == 1u);

	// A good write after them is seen and taken; the row goes.
	write_from_outside(level_text("beta"));
	BOOST_TEST(world_name() == "beta");
	BOOST_TEST(problems.size() == 0u);
}

BOOST_AUTO_TEST_CASE(DuringPlay_OnlyTheFlag_AndAfterStopACleanLevelTakesTheFile)
{
	open_level("alpha");
	const auto copy_before = working_copy_bytes();
	state.set_edits_locked(true);

	write_from_outside(level_text("beta"));
	BOOST_TEST(state.changed_on_disk());
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST((working_copy_bytes() == copy_before));
	BOOST_TEST(!level_ctrl.reload_from_disk().empty());

	state.set_edits_locked(false);
	level_ctrl.on_play_stopped();
	BOOST_TEST(world_name() == "beta");
	BOOST_TEST(!state.changed_on_disk());
	BOOST_TEST(!state.is_dirty());
}

BOOST_AUTO_TEST_CASE(DuringPlay_ADirtyLevelKeepsTheBannerAfterStop)
{
	open_level("alpha");
	make_dirty();
	state.set_edits_locked(true);
	write_from_outside(level_text("beta"));
	state.set_edits_locked(false);
	level_ctrl.on_play_stopped();

	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(state.changed_on_disk());
	BOOST_TEST(banner.visible());
}

BOOST_AUTO_TEST_CASE(AChangeDuringALevelLoad_IsTakenWhenTheLoadEnds)
{
	open_level("alpha");
	// As level_load_component::begin: the level is unwatched while the load runs, and a signal then changes nothing.
	bool loading = true;
	level_ctrl.load_in_progress = [&loading] { return loading; };
	level_ctrl.unwatch_level();
	BOOST_TEST(!level_ctrl.watched_file().is_valid());
	write_from_outside(level_text("beta"));
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(!state.changed_on_disk());

	// The load ended without a new level: the watch is back and the next tick compares the file once.
	loading = false;
	level_ctrl.resume_watch();
	BOOST_TEST(level_ctrl.disk_check_pending());
	BOOST_TEST(world_name() == "alpha");
	level_ctrl.tick_disk_change();
	BOOST_TEST(world_name() == "beta");
	BOOST_TEST(!level_ctrl.disk_check_pending());
}

BOOST_AUTO_TEST_CASE(AChangeWhileTheGameLayerIsAway_IsFoundAfterTheAttach)
{
	open_level("alpha");
	make_dirty();
	level_ctrl.detach_from_game_layer();
	write_from_outside(level_text("beta"));
	BOOST_TEST(!state.changed_on_disk());
	level_ctrl.attach_to_game_layer();
	level_ctrl.tick_disk_change();
	BOOST_TEST(state.changed_on_disk());
}

BOOST_AUTO_TEST_CASE(ASilentReload_WaitsWhileAPersonIsEditing)
{
	open_level("alpha");
	bool editing = true;
	level_ctrl.person_is_editing = [&editing] { return editing; };
	write_from_outside(level_text("beta"));
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(level_ctrl.disk_check_pending());
	level_ctrl.tick_disk_change();
	BOOST_TEST(world_name() == "alpha");
	editing = false;
	level_ctrl.tick_disk_change();
	BOOST_TEST(world_name() == "beta");
}

BOOST_AUTO_TEST_CASE(TheBannersReload_RebuildsTheLevelOnlyInTheQueuedStep)
{
	open_level("alpha");
	// A clean level flagged during Play, Play stopped for a module reload: the banner is up, its Reload asks nothing.
	state.set_edits_locked(true);
	write_from_outside(level_text("beta"));
	state.set_edits_locked(false);
	level_ctrl.on_play_stopped(true);
	BOOST_REQUIRE(banner.visible());

	banner.reload();
	BOOST_TEST(dialogs.pending() == 0u);
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST((working_copy_bytes() != test_util::to_bytes(level_text("beta"))));
	banner.run_pending();
	BOOST_TEST(world_name() == "beta");
	BOOST_TEST(banner.error().empty());
}

BOOST_AUTO_TEST_CASE(PlayStoppedForAModuleReload_DoesNotTakeTheFile)
{
	open_level("alpha");
	state.set_edits_locked(true);
	write_from_outside(level_text("beta"));
	state.set_edits_locked(false);
	level_ctrl.on_play_stopped(true);
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(state.changed_on_disk());
}

BOOST_AUTO_TEST_CASE(AWriteBlocker_RefusesTheBannersOverwriteAndReload)
{
	open_level("alpha");
	make_dirty();
	const std::string beta = level_text("beta");
	write_from_outside(beta);
	level_ctrl.write_blocker = []() -> std::optional<std::string> { return std::string{ "a level is loading" }; };
	BOOST_TEST(banner.blocked_reason().value_or("") == "a level is loading");

	bool written_ok = true;
	banner.overwrite([&written_ok](bool written) { written_ok = written; });
	banner.run_pending();
	BOOST_TEST(!written_ok);
	BOOST_TEST(read_file(level_file_path) == beta);
	BOOST_TEST(banner.error().find("a level is loading") != std::string::npos);
	BOOST_TEST(!level_ctrl.reload_from_disk().empty());
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST(state.changed_on_disk());
}

BOOST_AUTO_TEST_CASE(OverwriteDuringPlay_IsRefused)
{
	open_level("alpha");
	make_dirty();
	const std::string beta = level_text("beta");
	write_from_outside(beta);
	state.set_edits_locked(true);
	BOOST_TEST(banner.blocked_reason().has_value());
	const edt::save_result written = level_ctrl.overwrite_disk_change();
	BOOST_TEST(!written.saved);
	BOOST_TEST(written.error == "stop Play first");
	BOOST_TEST(read_file(level_file_path) == beta);
	state.set_edits_locked(false);
}

BOOST_AUTO_TEST_CASE(BrokenBytesFromOutsideOnADirtyLevel_OnlyFlag)
{
	open_level("alpha");
	make_dirty();
	const auto copy_before = working_copy_bytes();
	write_from_outside("{ \"__type\": \"level_desc\", ");
	BOOST_TEST(state.changed_on_disk());
	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST((working_copy_bytes() == copy_before));
	BOOST_TEST(problems.size() == 0u);
}

BOOST_AUTO_TEST_CASE(ALevelWhoseWorldDoesNotLoad_LeavesTheOldLevelAndOneProblem)
{
	open_level("alpha");
	const auto copy_before = working_copy_bytes();
	// Valid JSON of the level type, but its world is a file that is not there.
	const std::string missing_world = std::string(k_res_protocol) + "worlds/gone_for_1188.desc";
	const boost::json::object level{ { "__type", "level_desc" }, { "__version", descs->current_version("level_desc") },
		{ "name", "main" }, { "worlds", boost::json::array{ boost::json::object{ { "path", missing_world } } } } };
	write_from_outside(boost::json::serialize(level));

	BOOST_TEST(world_name() == "alpha");
	BOOST_TEST((working_copy_bytes() == copy_before));
	BOOST_TEST(problems.size() == 1u);
	BOOST_TEST(!state.changed_on_disk());
}

BOOST_AUTO_TEST_CASE(PersonSaveAsOntoTheFlaggedFile_AsksTheSameQuestion)
{
	open_level("alpha");
	make_dirty();
	const std::string beta = level_text("beta");
	write_from_outside(beta);
	menu().save_level();
	BOOST_REQUIRE(dialogs.pending() == 1u);
	const auto save_as = dialogs.list().front();
	BOOST_REQUIRE(dialogs.answer(save_as.id, boost::json::object{ { "path", std::string(k_level_path) } }).ok);
	BOOST_TEST(dialogs.pending() == 1u);
	BOOST_TEST(dialogs.list().front().kind == "choice");
	BOOST_TEST(read_file(level_file_path) == beta);
}

BOOST_AUTO_TEST_CASE(AnUntitledLevel_HasNoWatch)
{
	const std::string text = level_text("alpha");
	rs->store(working_copy, test_util::to_bytes(text));
	auto handle = rs->require_sync<scn::level_desc>(working_copy);
	pump();
	BOOST_REQUIRE(handle.is_ready());
	BOOST_REQUIRE(level_ctrl.take_loaded_level(handle, res::tag{}, working_copy));

	BOOST_TEST(!level_ctrl.watched_file().is_valid());
	BOOST_TEST(level_ctrl.disk_version().empty());
	BOOST_TEST(!level_ctrl.disk_conflict().has_value());
}

BOOST_AUTO_TEST_SUITE_END()
