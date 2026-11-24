#include <boost/test/unit_test.hpp>
#include <snk/editor/edt_problem_list.h>
#include "test_scene_env.hpp"
#include "test_temp_dir.hpp"
#include "autosave/edt_autosave_component.h"
#include "autosave/edt_autosave_files.h"
#include "autosave/edt_autosave_settings.h"
#include "edt_data_document.h"
#include "edt_level_controller.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/common/hash/sha256.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_system.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_job_service.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/resolvers/res_vfs_resolver.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_model.h>
#include <glm/gtc/matrix_transform.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Autosave (#1186): the dirty documents are written into cache://autosave/ an interval after their first unsaved edit,
// with a meta file; the source and the document stay as they are, and the files go when the document is clean or closed.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace std::chrono_literals;

constexpr const char* k_settings_schema = R"({
	"name": "test.autosave_settings", "kind": "desc", "is_document": true,
	"fields": [ { "name": "speed", "kind": "f32", "default": 1.0 } ]
})";

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
}

struct autosave_fixture : test_util::scene_env
{
	autosave_fixture()
	{
		fs::create_directories(content);
		fs::create_directories(cache);
		rs->unregistrate_resolver(res::tag::default_protocol());
		rs->registrate_resolver<res::vfs_resolver>(res::tag::default_protocol(), std::vector<fs::path>{ content },
			res::mount_access::read_write, res::write_policy::allow_all());
		BOOST_REQUIRE(rs->add_mount(res::mount{ res::tag::cache, { cache }, res::mount_access::read_write }));
		descs->register_schema(desc::parse_type_schema(json::parse(k_settings_schema).as_object()));

		hot = std::make_shared<scn::hot_reload_manager>(*assembler, *rs);
		mgr = std::make_shared<scn::context_manager>(*rs, *assembler, sf, hot);
		services.ctx_manager = mgr;
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		state.editor_tag = tag_of("level.desc");
		state.set_autosave_capture([this] { return level_ctrl.capture_autosave(); });
		state.set_save_handlers([this] { return level_ctrl.quick_save_level(); }, [this](const res::tag& to) {
			return level_ctrl.save_level_as(std::string(to.relative()));
		});
		autosave = make_autosave();
	}

	~autosave_fixture()
	{
		autosave.reset();
		documents.shutdown();
		rs->unregistrate_resolver(res::tag::cache);
		rs->unregistrate_resolver(res::tag::default_protocol());
	}

	std::unique_ptr<edt::autosave_component> make_autosave(edt::job_service* jobs = nullptr)
	{
		auto made = std::make_unique<edt::autosave_component>(edt::autosave_component::ports{ .documents = documents, .events = events,
			.resources = *rs, .jobs = jobs, .gesture_active = [this] { return gesture; }, .problems = &problems });
		made->set_clock_for_tests([this] { return now; });
		return made;
	}

	edt::desc_file_layout layout()
	{
		edt::desc_file_layout out;
		out.top_layer = [this] { return content; };
		out.layers = [this] { return std::vector<fs::path>{ content }; };
		out.open_level_files = [] { return std::vector<std::string>{}; };
		return out;
	}

	// One frame `seconds` after the last one.
	void after(std::chrono::seconds seconds)
	{
		now += seconds;
		autosave->tick();
	}

	void edit_level() { state.commit("edit", edt::commit_mode::state_only); }

	// The level saved once as levels/a.desc, so it has a source on disk.
	void save_level()
	{
		edit_level();
		BOOST_REQUIRE(level_ctrl.save_level_as("levels/a.desc"));
		BOOST_REQUIRE(!state.is_dirty());
	}

	edt::data_document& open_data()
	{
		std::ofstream(content / "settings.desc", std::ios::binary) << R"({"__type":"test.autosave_settings","speed":2.0})";
		auto made = std::make_unique<edt::data_document>(services, layout());
		const edt::open_result opened = made->open(res::tag::make("settings.desc"));
		BOOST_REQUIRE_MESSAGE(opened.opened, opened.error);
		return dynamic_cast<edt::data_document&>(documents.adopt(std::move(made)));
	}

	static bool set_speed(edt::data_document& document, double speed)
	{
		return document.edit([speed](json::value& shown) {
			shown.as_object()["speed"] = speed;
			return true;
		});
	}

	fs::path file_of(std::string_view relative) const { return cache / fs::path(std::string(relative)); }
	fs::path meta_of(std::string_view relative) const { return cache / fs::path(edt::autosave_meta_of(relative)); }
	bool written(std::string_view relative) const { return fs::exists(file_of(relative)) && fs::exists(meta_of(relative)); }
	bool nothing_at(std::string_view relative) const { return !fs::exists(file_of(relative)) && !fs::exists(meta_of(relative)); }

	static constexpr std::string_view k_untitled = "autosave/scene/untitled";
	static constexpr std::string_view k_level = "autosave/scene/levels%2Fa.desc";
	static constexpr std::string_view k_data = "autosave/data/settings.desc";

	test_util::temp_dir dir{ "edt_autosave" };
	const fs::path content = dir.path() / "content";
	const fs::path cache = dir.path() / "cache";
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	std::shared_ptr<scn::context_manager> mgr;
	edt::editor_services services;
	edt::editor_events events;
	edt::scene_document state;
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	edt::document_manager documents{ state, events };
	edt::problem_list problems;
	bool gesture = false;
	edt::autosave_component::clock::time_point now{ std::chrono::hours(1) };
	std::unique_ptr<edt::autosave_component> autosave;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtAutosave, autosave_fixture)

BOOST_AUTO_TEST_CASE(ADirtyDocument_IsWrittenAfterTheInterval_ACleanOneNever)
{
	autosave->tick();
	after(120s);
	BOOST_TEST(nothing_at(k_untitled));
	BOOST_TEST(autosave->writes() == 0u);

	edit_level();
	after(1s);
	after(58s);
	BOOST_TEST(nothing_at(k_untitled));
	after(2s);
	BOOST_TEST(written(k_untitled));
	BOOST_TEST(autosave->writes() == 1u);
	BOOST_TEST(autosave->written_file(state.id()) == std::string(k_untitled));
}

// Review of #1329: a document that is not autosaved says so in Problems until it is written again.
BOOST_AUTO_TEST_CASE(AWriteThatFails_IsAProblemsWarning_UntilTheNextWriteSucceeds)
{
	// A file where the kind's folder goes: the folder cannot be made.
	fs::create_directories(cache / "autosave");
	std::ofstream(cache / "autosave" / "scene", std::ios::binary) << "x";
	edit_level();
	after(1s);
	after(60s);
	BOOST_TEST(autosave->writes() == 0u);
	std::vector<edt::sourced_problem> listed = problems.list();
	BOOST_REQUIRE(listed.size() == 1u);
	BOOST_TEST(listed[0].source == std::string(edt::autosave_component::k_problem_source));
	BOOST_TEST((listed[0].item.severity == edt::problem_severity::warning));
	BOOST_TEST(listed[0].item.message.find("could not be autosaved") != std::string::npos);

	fs::remove(cache / "autosave" / "scene");
	edit_level();
	after(1s);
	after(60s);
	BOOST_TEST(written(k_untitled));
	BOOST_TEST(problems.list().empty());
}

BOOST_AUTO_TEST_CASE(TheCountdown_StartsAtTheFirstEdit_LaterEditsDoNotPushItBack)
{
	edit_level();
	after(1s);
	after(40s);
	edit_level();
	after(15s);
	BOOST_TEST(nothing_at(k_untitled));
	after(6s);
	BOOST_TEST(written(k_untitled));
}

BOOST_AUTO_TEST_CASE(NoSecondWrite_WithoutANewEdit_TheNextOneIsAnIntervalAfterIt)
{
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(autosave->writes() == 1u);
	for (int frame = 0; frame < 5; ++frame) after(120s);
	BOOST_TEST(autosave->writes() == 1u);

	edit_level();
	after(1s);
	after(30s);
	BOOST_TEST(autosave->writes() == 1u);
	after(31s);
	BOOST_TEST(autosave->writes() == 2u);
}

BOOST_AUTO_TEST_CASE(AGestureInProgress_DefersTheWrite_UntilItEnds)
{
	edit_level();
	after(1s);
	gesture = true;
	after(61s);
	after(30s);
	BOOST_TEST(nothing_at(k_untitled));

	gesture = false;
	after(0s);
	BOOST_TEST(written(k_untitled));
}

BOOST_AUTO_TEST_CASE(Save_RemovesTheAutosaveAndItsMeta)
{
	save_level();
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_level));

	BOOST_REQUIRE(state.save().saved);
	after(1s);
	BOOST_TEST(nothing_at(k_level));
	BOOST_TEST(autosave->written_file(state.id()).empty());
}

BOOST_AUTO_TEST_CASE(Discard_RemovesTheAutosaveAndItsMeta)
{
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_untitled));

	state.mark_clean();
	after(1s);
	BOOST_TEST(nothing_at(k_untitled));
}

BOOST_AUTO_TEST_CASE(Close_RemovesTheAutosaveAndItsMeta)
{
	edt::data_document& data = open_data();
	const edt::document_id id = data.id();
	BOOST_REQUIRE(set_speed(data, 5.0));
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_data));

	documents.close(id);
	BOOST_TEST(nothing_at(k_data));
	after(1s);
	BOOST_TEST(nothing_at(k_data));
}

BOOST_AUTO_TEST_CASE(ACleanExit_RemovesWhatWasWritten)
{
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_untitled));

	autosave->shutdown();
	BOOST_TEST(nothing_at(k_untitled));
}

BOOST_AUTO_TEST_CASE(AnAutosave_ChangesNeitherTheSourceNorTheDocument)
{
	save_level();
	const std::string source = read_file(content / "levels" / "a.desc");
	edit_level();
	const std::uint64_t generation = state.generation();
	const bool had_undo = state.next_undo_entry().has_value();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_level));

	BOOST_TEST(read_file(content / "levels" / "a.desc") == source);
	BOOST_TEST(state.is_dirty());
	BOOST_TEST(state.generation() == generation);
	BOOST_TEST(state.next_undo_entry().has_value() == had_undo);
}

BOOST_AUTO_TEST_CASE(DisabledInTheSettings_NothingIsWritten)
{
	autosave->set_settings({ .enabled = false, .interval_seconds = 60 });
	edit_level();
	after(1s);
	after(600s);
	BOOST_TEST(nothing_at(k_untitled));

	autosave->set_allowed(false);
	autosave->set_settings({});
	after(600s);
	BOOST_TEST(nothing_at(k_untitled));
}

BOOST_AUTO_TEST_CASE(TurningAutosaveOff_RemovesWhatItWrote)
{
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_untitled));

	autosave->set_settings({ .enabled = false, .interval_seconds = 60 });
	after(1s);
	BOOST_TEST(nothing_at(k_untitled));

	autosave->set_settings({});
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_untitled));
	autosave->set_allowed(false);
	after(1s);
	BOOST_TEST(nothing_at(k_untitled));
}

BOOST_AUTO_TEST_CASE(TheInterval_ComesFromTheSettings)
{
	autosave->set_settings({ .enabled = true, .interval_seconds = 10 });
	edit_level();
	after(1s);
	after(9s);
	BOOST_TEST(nothing_at(k_untitled));
	after(2s);
	BOOST_TEST(written(k_untitled));
}

BOOST_AUTO_TEST_CASE(InPlay_NothingIsWritten_UntilEditsAreUnlocked)
{
	edit_level();
	after(1s);
	state.set_edits_locked(true);
	after(61s);
	after(60s);
	BOOST_TEST(nothing_at(k_untitled));

	state.set_edits_locked(false);
	after(1s);
	BOOST_TEST(written(k_untitled));
}

BOOST_AUTO_TEST_CASE(TheLevelsAutosave_IsWhatSaveWrites)
{
	save_level();
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_level));
	const std::string autosaved = read_file(file_of(k_level));

	BOOST_REQUIRE(state.save().saved);
	BOOST_TEST(read_file(content / "levels" / "a.desc") == autosaved);
}

BOOST_AUTO_TEST_CASE(ADataDocumentsAutosave_IsWhatSaveWrites)
{
	edt::data_document& data = open_data();
	BOOST_REQUIRE(set_speed(data, 5.0));
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_data));
	const std::string autosaved = read_file(file_of(k_data));
	BOOST_TEST(read_file(content / "settings.desc").find("2.0") != std::string::npos);
	BOOST_TEST(data.is_dirty());

	const edt::save_result saved = data.save();
	BOOST_REQUIRE_MESSAGE(saved.saved, saved.error);
	BOOST_TEST(read_file(content / "settings.desc") == autosaved);
	after(1s);
	BOOST_TEST(nothing_at(k_data));
}

BOOST_AUTO_TEST_CASE(TheMetaFile_HasTheDocumentedFields)
{
	save_level();
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_level));

	const json::object meta = json::parse(read_file(meta_of(k_level))).as_object();
	BOOST_TEST(meta.at("format").to_number<int>() == 1);
	BOOST_TEST(std::string(meta.at("source").as_string()) == "levels/a.desc");
	BOOST_TEST(std::string(meta.at("kind").as_string()) == "scene");
	BOOST_TEST(meta.at("generation").to_number<std::uint64_t>() == state.generation());
	const std::string saved_at(meta.at("saved_at").as_string());
	BOOST_TEST(saved_at.size() == 20u);
	BOOST_TEST(saved_at.back() == 'Z');
	BOOST_TEST(saved_at[10] == 'T');
	const fs::path source = content / "levels" / "a.desc";
	BOOST_TEST(meta.at("source_size").to_number<std::uint64_t>() == fs::file_size(source));
	BOOST_TEST(std::string(meta.at("source_sha256").as_string()) == cmn::sha256_file_hex(source).value_or(""));

	const std::string payload = read_file(file_of(k_level));
	BOOST_TEST(meta.at("payload_size").to_number<std::uint64_t>() == payload.size());
	const auto* first = reinterpret_cast<const std::byte*>(payload.data());
	BOOST_TEST(std::string(meta.at("payload_sha256").as_string()) == cmn::sha256_hex(std::span<const std::byte>(first, payload.size())));
	BOOST_TEST(std::string(meta.at("session").as_string()) == edt::autosave_session_id());
	BOOST_TEST(edt::autosave_session_id().size() == 32u);

	const std::optional<edt::autosave_meta> parsed = edt::parse_autosave_meta(read_file(meta_of(k_level)));
	BOOST_REQUIRE(parsed.has_value());
	BOOST_TEST(parsed->kind == "scene");
	BOOST_TEST(parsed->source == "levels/a.desc");
}

BOOST_AUTO_TEST_CASE(AnUntitledLevelsMeta_HasAnEmptySourceAndNoSourceHash)
{
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_untitled));

	const json::object meta = json::parse(read_file(meta_of(k_untitled))).as_object();
	BOOST_TEST(std::string(meta.at("source").as_string()).empty());
	BOOST_TEST(!meta.contains("source_size"));
	BOOST_TEST(!meta.contains("source_sha256"));
}

// A project switch while a write is in flight: the write lands in the old cache folder and is removed from there; the
// new folder gets nothing that belongs to the old run.
BOOST_AUTO_TEST_CASE(AProjectSwitch_DuringAWrite_LeavesNothingUnderEitherFolder)
{
	edt::job_service jobs{ tasks };
	autosave = make_autosave(&jobs);
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(jobs.running_count() == 1u);
	BOOST_TEST(nothing_at(k_untitled));

	const fs::path other = dir.path() / "other_cache";
	fs::create_directories(other);
	BOOST_REQUIRE(rs->remount(res::tag::cache, { other }));
	after(1s);
	BOOST_TEST(autosave->cache_root() == other);
	pump();

	BOOST_TEST(nothing_at(k_untitled));
	BOOST_TEST(!fs::exists(other / "autosave" / "scene" / "untitled"));
	BOOST_TEST(autosave->written_file(state.id()).empty());

	// The next write goes into the new folder, an interval after the switch.
	after(61s);
	pump();
	BOOST_TEST(fs::exists(other / "autosave" / "scene" / "untitled"));
	autosave.reset();
	BOOST_TEST(!fs::exists(other / "autosave" / "scene" / "untitled"));
}

BOOST_AUTO_TEST_CASE(AWriteFinishedBeforeTheSwitchIsSeen_IsRemovedFromTheOldFolder)
{
	edt::job_service jobs{ tasks };
	autosave = make_autosave(&jobs);
	edit_level();
	after(1s);
	after(61s);
	const fs::path other = dir.path() / "other_cache";
	fs::create_directories(other);
	BOOST_REQUIRE(rs->remount(res::tag::cache, { other }));
	pump();
	BOOST_REQUIRE(written(k_untitled));

	after(1s);
	BOOST_TEST(nothing_at(k_untitled));
	BOOST_TEST(!fs::exists(other / "autosave"));
	autosave.reset();
}

BOOST_AUTO_TEST_CASE(Shutdown_DropsAWriteNotStarted_AndNoCallbackRunsAfterIt)
{
	edt::job_service jobs{ tasks };
	autosave = make_autosave(&jobs);
	edit_level();
	after(1s);
	after(61s);
	autosave->shutdown();
	pump();
	BOOST_TEST(nothing_at(k_untitled));
	BOOST_TEST(autosave->writes() == 0u);
	autosave.reset();
}

// A link where autosave writes: a folder link is refused, a file link is replaced, never followed.
BOOST_AUTO_TEST_CASE(ALinkUnderTheAutosaveFolder_IsNeverFollowed)
{
	const fs::path outside = dir.path() / "outside";
	fs::create_directories(outside);
	fs::create_directories(cache / "autosave");
	std::error_code ec;
	fs::create_directory_symlink(outside, cache / "autosave" / "scene", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	edit_level();
	after(1s);
	after(61s);
	BOOST_TEST(fs::is_empty(outside));
	BOOST_TEST(autosave->writes() == 0u);

	fs::remove(cache / "autosave" / "scene");
	fs::create_directories(cache / "autosave" / "scene");
	std::ofstream(outside / "target.txt", std::ios::binary) << "keep";
	fs::create_symlink(outside / "target.txt", cache / "autosave" / "scene" / "untitled", ec);
	BOOST_REQUIRE(!ec);
	edit_level();
	after(1s);
	after(61s);
	BOOST_TEST(read_file(outside / "target.txt") == "keep");
}

// A live world whose entity was moved in the ECS and not yet written back: autosave takes the move on a copy (the document
// keeps its desc) and writes what Save writes once it has synced the move itself.
BOOST_AUTO_TEST_CASE(ALiveLevelsAutosave_TakesTheEcsTransforms_AndIsWhatSaveWrites)
{
	const res::tag tag = put("lv.desc", R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
		{"__type":"world_desc","__version":5,"name":"w0","systems":[],"children":[
			{"__id":"00000000000000a2","name":"Crate","components":{"stat":{"__type":"stat_desc","hp":3}}}]}]})");
	state.editor_tag = tag;
	auto handle = rs->require_sync<scn::level_desc>(tag);
	BOOST_REQUIRE(level_ctrl.finish_level_load(handle));
	BOOST_REQUIRE(state.has_active_world());
	save_level();

	entt::registry& reg = mgr->level_set().get("w0").state();
	bool moved = false;
	reg.view<scn::node_id_component, scn::local_transform>().each([&moved](const scn::node_id_component& id, scn::local_transform& lt) {
		if (id.path == std::vector<std::uint64_t>{ 0xa2 }) {
			lt.local = glm::translate(glm::mat4{ 1.f }, glm::vec3{ 5.f, 0.f, 0.f });
			moved = true;
		}
	});
	BOOST_REQUIRE(moved);
	edit_level();
	after(1s);
	after(61s);
	BOOST_REQUIRE(written(k_level));
	const std::string autosaved = read_file(file_of(k_level));
	BOOST_TEST(state.world_descs[0].get_root().children[0].position.x == 0.f);
	BOOST_TEST(autosaved != read_file(content / "levels" / "a.desc"));

	BOOST_REQUIRE(state.save().saved);
	BOOST_TEST(state.world_descs[0].get_root().children[0].position.x == 5.f);
	BOOST_TEST(read_file(content / "levels" / "a.desc") == autosaved);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(EdtAutosaveFiles)

// No project: the resource system has no cache:// mount, and autosave writes nothing.
BOOST_AUTO_TEST_CASE(WithoutACacheMount_AutosaveIsOff)
{
	test_util::ram_res_env env;
	BOOST_REQUIRE(env.rs->mount_roots(res::tag::cache).empty());
	edt::editor_events events;
	edt::scene_document scene;
	edt::document_manager documents{ scene, events };
	edt::autosave_component::clock::time_point now{ std::chrono::hours(1) };
	edt::autosave_component autosave{ edt::autosave_component::ports{ .documents = documents, .events = events, .resources = *env.rs,
		.jobs = nullptr, .gesture_active = {}, .problems = nullptr } };
	autosave.set_clock_for_tests([&now] { return now; });

	scene.commit("edit", edt::commit_mode::state_only);
	autosave.tick();
	now += 600s;
	autosave.tick();
	BOOST_TEST(autosave.cache_root().empty());
	BOOST_TEST(autosave.writes() == 0u);
	documents.shutdown();
}


BOOST_AUTO_TEST_CASE(AFileName_IsOneNamePerSource)
{
	BOOST_TEST(edt::autosave_file_of("scene", res::tag{}) == "autosave/scene/untitled");
	BOOST_TEST(edt::autosave_file_of("prefab", res::tag::make("prefabs/a.prefab.desc")) == "autosave/prefab/prefabs%2Fa.prefab.desc");
	BOOST_TEST(edt::autosave_file_of("data", res::tag::make("a%b/c:d.desc")) == "autosave/data/a%25b%2Fc%3Ad.desc");
	BOOST_TEST(edt::autosave_file_of("data", res::tag::make(".hidden.desc")) == "autosave/data/%2Ehidden.desc");
	const std::string long_name = edt::autosave_file_of("data", res::tag::make(std::string(400, 'x') + ".desc"));
	BOOST_TEST(long_name.size() < 200u);
	BOOST_TEST(long_name != edt::autosave_file_of("data", res::tag::make("y" + std::string(399, 'x') + ".desc")));
	// U+00E9 is two bytes: a cut never starts inside a character or inside a %XX escape.
	std::string accents;
	for (int i = 0; i < 150; ++i) accents += i % 7 == 0 ? std::string{ "/" } : std::string{ "\xC3\xA9" };
	for (const std::string& path : { accents, "x" + accents }) {
		const std::string name = edt::autosave_file_of("data", res::tag::make(path));
		const std::string leaf = name.substr(std::string_view{ "autosave/data/" }.size());
		BOOST_TEST(leaf.size() <= 160u);
		BOOST_TEST((static_cast<unsigned char>(leaf.front()) & 0xC0) != 0x80);
		BOOST_TEST(!(leaf.front() >= '0' && leaf.front() <= '9'));
		BOOST_TEST(leaf.find('/') == std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(TheMeta_ReadsBackAsWritten)
{
	edt::autosave_meta meta;
	meta.source = "levels/a.desc";
	meta.kind = "scene";
	meta.generation = 42;
	meta.saved_at = "2026-11-24T10:15:00Z";
	meta.source_size = 18;
	meta.source_sha256 = std::string(64, 'a');
	meta.session = edt::autosave_session_id();
	meta.payload_size = 7;
	meta.payload_sha256 = std::string(64, 'b');
	const std::optional<edt::autosave_meta> parsed = edt::parse_autosave_meta(edt::autosave_meta_text(meta));
	BOOST_REQUIRE(parsed.has_value());
	BOOST_TEST((*parsed == meta));
	BOOST_TEST(!edt::parse_autosave_meta(R"({"format":2,"source":"","kind":"scene","generation":1,"saved_at":"x"})").has_value());
}

BOOST_AUTO_TEST_CASE(TheSettings_ParseClampAndRefuse)
{
	const edt::autosave_settings_result parsed = edt::parse_autosave_settings(R"({"enabled": false, "interval_seconds": 2})");
	BOOST_TEST(parsed.error.empty());
	BOOST_TEST(!parsed.settings.enabled);
	BOOST_TEST(parsed.settings.interval_seconds == edt::k_autosave_min_interval);
	BOOST_TEST(!edt::parse_autosave_settings(R"({"enabled": 1})").error.empty());
	BOOST_TEST(!edt::parse_autosave_settings(R"({"other": true})").error.empty());
	const edt::autosave_settings round = edt::parse_autosave_settings(edt::serialize_autosave_settings({ .enabled = true, .interval_seconds = 90 })).settings;
	BOOST_TEST(round.interval_seconds == 90);
	BOOST_TEST(round.enabled);
}

BOOST_AUTO_TEST_SUITE_END()
