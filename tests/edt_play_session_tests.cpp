#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include "null_engine_fixture.hpp"
#include "edt_agent_sessions.h"
#include "edt_camera_redirect.h"
#include "edt_edit_menu.h"
#include "edt_editor_camera.h"
#include "edt_editor_mode.h"
#include "edt_editor_mode_commands.h"
#include "edt_input_target.h"
#include "edt_level_controller.h"
#include "edt_module_swap_gate.h"
#include "edt_play_commands.h"
#include "edt_play_session.h"
#include "edt_agent_access.h"
#include "edt_agent_confirmations.h"
#include "edt_mcp_bridge_host.h"
#include "test_temp_dir.hpp"
#include "edt_scene_commands.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/application/app_module_source.h>
#include <snk/common/mem_allocator.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/core/core_project_modules.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_selection_service.h>
#include <snk/game/gs_game_module.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_input_router.h>
#include <snk/mcp/mcp_host.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/scene/scn_time.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Play of the scene document (#490, US-63-B): the set "play" is built from copies of the document, the level is
// hidden and the document locked while it runs, Stop brings the editor back as it was, and nothing is left behind.

namespace {

namespace json = boost::json;

struct play_game_desc : desc::desc_base {
	void deserialize(desc::desc_system&, const json::object&) override {}
	void serialize(json::object&) const override {}
};

// A build of the game module "play_game"; Build 2 registers one more type.
template<int Build>
class play_game_build final : public gs::game_module {
public:
	std::string_view name() const override { return "play_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		layer.descs().register_component_desc<play_game_desc>("play_game_desc");
		if constexpr (Build > 1) {
			layer.descs().register_component_desc<play_game_desc>("play_game_v2_desc");
		}
	}
};

using play_game_v1 = play_game_build<1>;
using play_game_v2 = play_game_build<2>;

}

SNK_DECLARE_MODULE_PREFIXED(play_game_v1_, play_game_v1)
SNK_DECLARE_MODULE_PREFIXED(play_game_v2_, play_game_v2)

namespace {

std::unique_ptr<app::modules::module_source> build_of(app::modules::module_entry entry)
{
	return std::make_unique<app::modules::static_module_source>("play_game", entry);
}

app::modules::module_entry v1_entry()
{
	return { &play_game_v1_module_api_version, &play_game_v1_module_build_info, &play_game_v1_create_module,
		&play_game_v1_destroy_module };
}

app::modules::module_entry v2_entry()
{
	return { &play_game_v2_module_api_version, &play_game_v2_module_build_info, &play_game_v2_create_module,
		&play_game_v2_destroy_module };
}

constexpr std::uint64_t cam_id = 0xa1;
constexpr std::uint64_t crate_id = 0xa2;

// w0: a camera that draws to the backbuffer (redirected in the editor) and a crate; w1: empty.
constexpr const char* LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":3,"name":"w0","systems":[],"children":[
		{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":1}}},
		{"__id":"00000000000000a2","name":"Crate","components":{"stat":{"__type":"stat_desc","hp":3}}}]},
	{"__type":"world_desc","__version":3,"name":"w1","systems":[]}]})";

// The same level after a change on disk: the crate is gone and a world was added.
constexpr const char* LEVEL_CHANGED = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":3,"name":"w0","systems":[],"children":[
		{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":1}}}]},
	{"__type":"world_desc","__version":3,"name":"w1","systems":[]},
	{"__type":"world_desc","__version":3,"name":"w2","systems":[]}]})";

// LEVEL with a system that runs only in Play (not in Edit, the level's mode) and holds a resource nothing else holds.
constexpr const char* PLAY_ONLY_SYSTEM = "test::hold_play_only_resource";
constexpr const char* HOLDING_LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":3,"name":"w0","systems":["test::hold_play_only_resource"],"children":[
		{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":1}}},
		{"__id":"00000000000000a2","name":"Crate","components":{"stat":{"__type":"stat_desc","hp":3}}}]},
	{"__type":"world_desc","__version":3,"name":"w1","systems":[]}]})";

// A context variable of the world: the handle goes with the world's registry.
struct play_only_holder
{
	res::res_handle<res::text_resource> handle;
};
res::resource_system* g_play_only_resources = nullptr;
res::tag g_play_only_tag;

void hold_play_only_resource(play_only_holder& holder)
{
	if (!holder.handle.is_valid() && g_play_only_resources) {
		holder.handle = g_play_only_resources->require<res::text_resource>(g_play_only_tag);
	}
}

// A level with a component of the module's desc type.
constexpr const char* MODULE_LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":3,"name":"w0","systems":[],"components":{"gc":{"__type":"play_game_desc","speed":3}}}]})";

using state_change = std::pair<edt::play_state, edt::play_reason>;

struct play_fixture : test_util::module_env
{
	play_fixture()
	{
		services.ctx_manager = root.require_shared<scn::context_manager>();
		services.base_descs = descs.get();
		services.base_resources = rs.get();
		services.base_assembler = assembler.get();
		services.base_sfactory = &sf;
		// What editor_system wires: the editor camera follows the active world, a switch is announced.
		const auto save_camera = [this](entt::registry& reg) { edt::save_camera_state(reg, state.camera); };
		const auto inject_camera = [this](entt::registry& reg) { edt::inject_camera(reg, state.camera); };
		scene_ed.on_save_camera = save_camera;
		scene_ed.on_inject_camera = inject_camera;
		world_ctrl.on_save_camera = save_camera;
		world_ctrl.on_inject_camera = inject_camera;
		world_ctrl.on_world_switched = [this](int) { events.active_world_changed.emit(state.id()); };
		contexts().set_on_reload([this] { level_ctrl.resync_worlds_after_reload(); });
		contexts().set_on_game_layer_change([this](scn::context_manager::game_layer_change change) {
			gate.on_game_layer_change(change == scn::context_manager::game_layer_change::before);
		});
		gate.set_play_hooks([this] { play.before_module_swap(); }, [this] { play.after_module_swap(); });
		contexts().level_set().set_mode(ecs::run_mode::edit);
	}

	~play_fixture()
	{
		play.stop();
		contexts().set_on_game_layer_change(nullptr);
		contexts().set_on_reload(nullptr);
	}

	res::tag open_level(const char* text = LEVEL)
	{
		const res::tag tag = put("lv.desc", text);
		state.editor_tag = tag;
		state.level_tag = tag;
		auto handle = services.resources()->require_sync<scn::level_desc>(tag);
		BOOST_REQUIRE(level_ctrl.finish_level_load(handle));
		return tag;
	}

	// The level file changes on disk; the hot reload notices it in the next update.
	void change_level_file(const char* text)
	{
		const res::tag tag = put("lv.desc", text);
		rs->signal_changed(tag);
		pump();
		contexts().update(std::chrono::duration<float>(0.016f));
	}

	void select(std::uint64_t node, std::string world = "w0")
	{
		edt::selection value;
		value.world = std::move(world);
		value.path = { node };
		selection.select(std::move(value));
	}

	std::string level_json() const { return json::serialize(level_ctrl.build_level_json()); }

	scn::context_set* play_set() { return contexts().find_set(edt::k_play_set); }
	scn::hot_reload_manager& layer_hot_reload() { return *contexts().game_layer()->hot_reload(); }

	static std::size_t editor_cameras(entt::registry& reg) { return reg.view<edt::editor_camera_tag>().size(); }

	edt::editor_services services;
	edt::scene_document state;
	edt::editor_events events;
	edt::document_manager documents{ state, services, events };
	edt::selection_service selection{ events };
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	inp::input_router router{ std::make_shared<inp::ecs_input_manager>() };
	// What editor_system's start_blocker says (a level or an asset loading); nullopt: nothing blocks.
	std::optional<std::string> start_blocked;
	edt::play_session play{ state, services, world_ctrl, selection, events, edt::play_session_options{ .input = &router,
		.start_blocker = [this] { return start_blocked; } } };
	std::vector<state_change> seen;
	edt::connection seen_connection = events.play_state_changed.subscribe([this](edt::play_state s, edt::play_reason r) {
		seen.emplace_back(s, r);
	});
	edt::module_swap_gate gate{ [this](bool before) {
		if (before) level_ctrl.detach_from_game_layer();
		else level_ctrl.attach_to_game_layer();
	} };
	core::project_module_host host{ loader, root, core::project_modules_options{} };
};

}

BOOST_FIXTURE_TEST_SUITE(edt_play_session_tests, play_fixture)

BOOST_AUTO_TEST_CASE(start_builds_the_play_set_from_copies_hides_the_level_and_changes_nothing_of_the_document)
{
	open_level();
	scene_ed.create_entity("Empty"); // unsaved: Play shows the level as edited
	BOOST_REQUIRE(state.is_dirty());
	const std::string before = level_json();
	const std::uint64_t generation = state.generation();
	const std::size_t attached = layer_hot_reload().attached_count();

	BOOST_REQUIRE(play.start());

	BOOST_TEST((play.state() == edt::play_state::playing));
	scn::context_set* set = play_set();
	BOOST_REQUIRE(set);
	BOOST_TEST(!set->hot_reload());
	BOOST_TEST(set->layer() == contexts().game_layer());
	BOOST_TEST((set->mode() == ecs::run_mode::play));
	BOOST_TEST(set->size() == 2u);
	set->for_each([](scn::world_context& world) { BOOST_TEST((world.kind() == scn::context_kind::play)); });
	BOOST_TEST(layer_hot_reload().attached_count() == attached); // nothing of the play set is hot reloaded
	// The unsaved entity is in the twin: built from the document, not from the file.
	bool has_entity = false;
	for (auto [ent, name] : set->get("w0").state().view<scn::name_component>().each()) {
		if (name.name == "Entity") has_entity = true;
	}
	BOOST_TEST(has_entity);

	scn::context_set& level = contexts().level_set();
	BOOST_TEST(!level.active());
	level.for_each([](scn::world_context& world) { BOOST_TEST(!world.extract()); });
	BOOST_TEST(editor_cameras(set->get("w0").state()) == 1u);
	BOOST_TEST(editor_cameras(set->get("w1").state()) == 0u);

	BOOST_TEST(state.edits_locked());
	BOOST_TEST(level_json() == before);
	BOOST_TEST(state.generation() == generation);
	BOOST_TEST(state.is_dirty());
	BOOST_TEST((seen == std::vector<state_change>{ { edt::play_state::playing, edt::play_reason::user } }));

	// The views and the input read the play set.
	BOOST_TEST(edt::view_set(services) == set);
	BOOST_TEST(edt::view_registry(services, state) == &set->get("w0").state());
	const auto target = edt::active_input_target(services, state);
	BOOST_REQUIRE(target.has_value());
	BOOST_TEST(target->set == "play");

	// A second start is refused, nothing changes.
	BOOST_TEST(play.start_refusal().has_value());
	BOOST_TEST(!play.start());
	BOOST_TEST(seen.size() == 1u);
}

BOOST_AUTO_TEST_CASE(switching_the_active_world_during_play_moves_the_editor_camera_to_the_other_twin)
{
	open_level();
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(world_ctrl.switch_to_world(1));

	BOOST_TEST(editor_cameras(play_set()->get("w0").state()) == 0u);
	BOOST_TEST(editor_cameras(play_set()->get("w1").state()) == 1u);
	BOOST_TEST(edt::view_registry(services, state) == &play_set()->get("w1").state());

	play.stop();
	BOOST_TEST(state.active_world_idx == 0); // the world active before Play
	BOOST_TEST(edt::view_registry(services, state) == &contexts().level_set().get("w0").state());
}

BOOST_AUTO_TEST_CASE(the_play_twins_start_their_shader_time_at_zero_on_every_start)
{
	open_level();
	const auto seconds = [](scn::world_context& world) { return world.state().ctx().get<scn::world_time>().seconds; };
	const auto frames = [this](int count) { for (int i = 0; i < count; ++i) contexts().update(std::chrono::duration<float>(0.016f)); };
	frames(5); // the level's time runs in Edit
	scn::world_context& level_w0 = contexts().level_set().get("w0");
	const double level_before = seconds(level_w0);
	BOOST_REQUIRE(level_before > 0.0);

	for (int session = 0; session < 2; ++session) {
		BOOST_REQUIRE(play.start());
		play_set()->for_each([&](scn::world_context& world) { BOOST_TEST(seconds(world) == 0.0); });
		frames(3);
		BOOST_TEST(seconds(play_set()->get("w0")) > 0.0);
		play.stop();
	}
	// the hidden level stood during Play and keeps its own time
	BOOST_TEST(seconds(contexts().level_set().get("w0")) == level_before);
}

BOOST_AUTO_TEST_CASE(stop_brings_the_editor_back_as_it_was_before_play)
{
	open_level();
	select(crate_id);
	const edt::editor_camera_state camera = state.camera;
	const std::string before = level_json();
	const bool dirty = state.is_dirty();

	BOOST_REQUIRE(play.start());
	// The person flies the camera in the twin and selects something else.
	entt::registry& twin = play_set()->get("w0").state();
	for (auto [ent, ctrl] : twin.view<edt::editor_camera_tag, scn::mouse_controller_component>().each()) {
		ctrl.position += glm::vec3(5.f, 0.f, 0.f);
	}
	select(cam_id);
	router.bind({ .set = "play", .context = "w0" }, glm::ivec4(0, 0, 10, 10));
	router.set_hold("play", { .active = true, .captured = true, .owner = { .set = "play", .context = "w0" } });
	play.stop();

	BOOST_TEST(play_set() == nullptr);
	BOOST_TEST((play.state() == edt::play_state::stopped));
	scn::context_set& level = contexts().level_set();
	BOOST_TEST(level.active());
	level.for_each([](scn::world_context& world) { BOOST_TEST(world.extract()); });
	BOOST_TEST(state.camera.position.x == camera.position.x);
	entt::registry& level_w0 = level.get("w0").state();
	BOOST_TEST(editor_cameras(level_w0) == 1u);
	for (auto [ent, ctrl] : level_w0.view<edt::editor_camera_tag, scn::mouse_controller_component>().each()) {
		BOOST_TEST(ctrl.position.x == camera.position.x);
	}
	BOOST_TEST((selection.current().path == std::vector<std::uint64_t>{ crate_id }));
	BOOST_TEST(selection.current().world == "w0");
	BOOST_TEST(!state.edits_locked());
	BOOST_TEST(level_json() == before);
	BOOST_TEST(state.is_dirty() == dirty);
	BOOST_TEST(router.binding_count() == 0u);
	BOOST_TEST(router.hold_count() == 0u);
	BOOST_TEST((seen == std::vector<state_change>{ { edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::stopped, edt::play_reason::user } }));
	BOOST_TEST(edt::view_set(services) == &level);
}

BOOST_AUTO_TEST_CASE(a_level_change_during_play_waits_for_stop_and_a_vanished_selection_is_deselected)
{
	open_level();
	select(crate_id);
	BOOST_REQUIRE(play.start());

	change_level_file(LEVEL_CHANGED);

	// Not while playing: the level stays as it was, its cameras stay hidden.
	BOOST_TEST(contexts().has_pending_level_reload());
	BOOST_TEST(contexts().level_set().find("w2") == nullptr);
	BOOST_TEST(state.world_names.size() == 2u);
	contexts().level_set().for_each([](scn::world_context& world) { BOOST_TEST(!world.extract()); });
	BOOST_TEST(play_set()->find("w2") == nullptr);

	play.stop();

	BOOST_TEST(!contexts().has_pending_level_reload());
	BOOST_TEST(contexts().level_set().find("w2") != nullptr);
	BOOST_TEST((state.world_names == std::vector<std::string>{ "w0", "w1", "w2" }), boost::test_tools::per_element());
	BOOST_TEST(!selection.has_selection()); // the crate is gone
	BOOST_TEST(editor_cameras(contexts().level_set().get("w0").state()) == 1u);
}

BOOST_AUTO_TEST_CASE(pause_resume_and_step_map_to_the_play_set)
{
	open_level();
	BOOST_TEST(!play.pause()); // stopped: nothing to pause
	BOOST_TEST(!play.step(1));
	BOOST_REQUIRE(play.start());
	scn::context_set& set = *play_set();

	BOOST_TEST(play.pause());
	BOOST_TEST((set.mode() == ecs::run_mode::pause));
	BOOST_TEST((play.state() == edt::play_state::paused));
	BOOST_TEST(!play.pause());

	BOOST_TEST(play.resume());
	BOOST_TEST((set.mode() == ecs::run_mode::play));
	BOOST_TEST(!play.resume());

	BOOST_TEST(play.step(3, edt::play_reason::agent));
	BOOST_TEST((set.mode() == ecs::run_mode::step));
	BOOST_TEST(set.pending_steps() == 3u);
	BOOST_TEST(play.pending_steps() == 3u);
	BOOST_TEST((play.state() == edt::play_state::paused));
	for (int frame = 0; frame < 3; ++frame) contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(play.pending_steps() == 0u);
	BOOST_TEST((set.mode() == ecs::run_mode::pause));

	BOOST_TEST(play.step(0)); // one frame at least
	BOOST_TEST(play.pending_steps() == 1u);

	play.set_time_scale(0.5f);
	BOOST_TEST(set.time_scale() == 0.5f);
	play.set_seed(77u);
	BOOST_TEST(set.seed() == 77u);

	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::paused, edt::play_reason::user },
		{ edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::paused, edt::play_reason::agent } }));

	// Kept for the next session.
	play.stop();
	BOOST_REQUIRE(play.start());
	BOOST_TEST(play_set()->time_scale() == 0.5f);
	BOOST_TEST(play_set()->seed() == 77u);
	BOOST_TEST((play_set()->mode() == ecs::run_mode::play));
}

BOOST_AUTO_TEST_CASE(edits_during_play_are_refused_but_saving_is_allowed)
{
	open_level();
	scene_ed.create_entity("Empty");
	BOOST_REQUIRE(state.history().can_undo());
	edt::action_registry actions;
	edt::dialog_service dialogs;
	dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	const auto edit_actions = edt::register_edit_actions(actions, documents, dialogs, nullptr);
	cmd::command_registry commands;
	const auto scene_commands = edt::register_scene_commands(commands, { documents, selection, services, nullptr });
	edt::agent_sessions sessions(documents);
	const auto session_commands = sessions.register_commands(commands);
	int saves = 0;
	state.set_save_handlers([&saves] { ++saves; return true; }, [&saves](const res::tag&) { ++saves; return true; });
	BOOST_REQUIRE(actions.is_enabled("edit.undo"));

	BOOST_REQUIRE(play.start());
	const std::string before = level_json();
	const std::uint64_t generation = state.generation();
	const std::size_t children = state.world_descs[0].get_root().children.size();

	state.commit("Edit node");
	scene_ed.create_entity("Empty");
	scene_ed.delete_entity(crate_id);
	world_ctrl.create_world("w9");
	BOOST_TEST(state.generation() == generation);
	BOOST_TEST(state.world_descs[0].get_root().children.size() == children);
	BOOST_TEST(state.world_names.size() == 2u);
	BOOST_TEST(contexts().level_set().find("w9") == nullptr);

	BOOST_TEST(!actions.is_enabled("edit.undo"));
	BOOST_TEST(actions.invoke("edit.undo", json::object{}).code == "disabled");
	BOOST_TEST(actions.invoke("edit.redo", json::object{}).code == "disabled");
	BOOST_TEST(!state.undo());

	const auto renamed = commands.invoke("scene.node.rename",
		json::parse(R"({"ref":{"context":"w0","path":["00000000000000a2"]},"name":"Box"})"), cmd::origin::agent("s1"));
	BOOST_TEST(renamed.code == "busy");
	BOOST_TEST(renamed.error.find("stop play first") != std::string::npos);
	const auto by_person = commands.invoke("scene.node.rename",
		json::parse(R"({"ref":{"context":"w0","path":["00000000000000a2"]},"name":"Box"})"), cmd::origin::ui());
	BOOST_TEST(by_person.code == "busy");
	const auto begun = commands.invoke("agent.session.begin", json::parse(R"({"label":"x"})"), cmd::origin::agent("s1"));
	BOOST_TEST(begun.code == "busy");
	BOOST_TEST(begun.error.find("stop play first") != std::string::npos);
	BOOST_TEST(commands.invoke("agent.session.get", json::object{}, cmd::origin::agent("s1")).ok);
	BOOST_TEST(level_json() == before);

	BOOST_TEST(state.can_save());
	BOOST_TEST(state.save().saved);
	BOOST_TEST(saves == 1);

	play.stop();
	BOOST_TEST(actions.is_enabled("edit.undo"));
	BOOST_TEST(commands.invoke("scene.node.rename",
		json::parse(R"({"ref":{"context":"w0","path":["00000000000000a2"]},"name":"Box"})"), cmd::origin::agent("s1")).ok);
}

BOOST_AUTO_TEST_CASE(ten_start_stop_cycles_leave_nothing_behind)
{
	g_play_only_resources = rs.get();
	g_play_only_tag = put("play_only.txt", "held by the play set only");
	BOOST_REQUIRE(contexts().game_layer()->systems().register_system<&hold_play_only_resource>(PLAY_ONLY_SYSTEM));
	open_level(HOLDING_LEVEL);
	edt::camera_redirector redirector;
	std::vector<std::string> released;
	const auto redirect = [&] {
		redirector.update(contexts(), [&released](const res::tag& tag) { released.emplace_back(tag.view()); });
	};
	redirect();

	struct counters
	{
		std::size_t sets = 0;
		std::size_t contexts = 0;
		std::size_t attached = 0;
		std::size_t base_attached = 0;
		std::size_t channel_types = 0;
		std::size_t live_resources = 0;
		std::size_t bindings = 0;
		std::size_t holds = 0;
		std::size_t redirected_targets = 0;
		bool operator==(const counters&) const = default;
	};
	const auto snapshot = [&] {
		counters c;
		contexts().for_each_set([&c](scn::context_set&) { ++c.sets; });
		contexts().for_each([&c](scn::world_context&) { ++c.contexts; });
		c.attached = layer_hot_reload().attached_count();
		c.base_attached = hot->attached_count();
		c.channel_types = contexts().channels().size();
		c.live_resources = rs->live_resources();
		c.bindings = router.binding_count();
		c.holds = router.hold_count();
		c.redirected_targets = redirector.tracked_count();
		return c;
	};
	const counters baseline = snapshot();
	BOOST_REQUIRE(baseline.redirected_targets == 1u); // the level's camera

	for (int cycle = 0; cycle < 10; ++cycle) {
		BOOST_TEST_CONTEXT("cycle " << cycle) {
			BOOST_REQUIRE(play.start());
			// What a frame of the editor makes while playing: a texture for the play camera, a binding of the Game view
			// and a mouse hold on the play twin.
			redirect();
			router.bind({ .set = "play", .context = "w0" }, glm::ivec4(0, 0, 10, 10));
			router.set_hold("play", { .active = true, .captured = true, .owner = { .set = "play", .context = "w0" } });
			for (int frame = 0; frame < 3; ++frame) contexts().update(std::chrono::duration<float>(0.016f));
			pump(); // the load of the play-only resource ends: only the play world holds it

			const counters playing = snapshot();
			BOOST_TEST(playing.live_resources > baseline.live_resources);
			BOOST_TEST(playing.sets == baseline.sets + 1);
			BOOST_TEST(playing.contexts == baseline.contexts + 2);
			BOOST_TEST(playing.redirected_targets == baseline.redirected_targets + 1);
			BOOST_TEST(playing.bindings == baseline.bindings + 1);
			BOOST_TEST(playing.holds == baseline.holds + 1);
			BOOST_TEST(playing.attached == baseline.attached);
			BOOST_TEST(playing.base_attached == baseline.base_attached);

			released.clear();
			play.stop();
			redirect(); // the editor's next frame
			BOOST_TEST((released == std::vector<std::string>{ "memory://__game_camera_rt_play" }), boost::test_tools::per_element());
			BOOST_TEST((snapshot() == baseline));
		}
	}
	const counters after = snapshot();
	BOOST_TEST(after.sets == baseline.sets);
	BOOST_TEST(after.contexts == baseline.contexts);
	BOOST_TEST(after.attached == baseline.attached);
	BOOST_TEST(after.channel_types == baseline.channel_types);
	BOOST_TEST(after.live_resources == baseline.live_resources);
	BOOST_TEST(after.redirected_targets == baseline.redirected_targets);
	BOOST_TEST(after.holds == 0u);
	BOOST_TEST(seen.size() == 20u);
	g_play_only_resources = nullptr;
}

BOOST_AUTO_TEST_CASE(a_continuation_of_a_play_task_does_not_run_after_stop)
{
	open_level();
	BOOST_REQUIRE(play.start());
	scn::scene_layer* layer = play_set()->layer();
	BOOST_REQUIRE(layer);
	entt::registry* twin = &play_set()->get("w0").state();
	bool continued = false;
	auto handle = tasks.run(ds::lane::compute, ds::task_desc{ .name = "a play task", .category = "test" },
		layer->tasks(), play_set()->tasks(), [](ds::task_context&) {});
	handle.then_main([&continued, twin](ds::task_result<void>&) { continued = twin->valid(entt::null); });
	BOOST_REQUIRE(tasks.run_one(ds::lane::compute)); // done: its continuation waits in the main queue

	play.stop();
	pump();

	BOOST_TEST(!continued);
	BOOST_TEST((handle.status() == ds::task_status::ready));
}

BOOST_AUTO_TEST_CASE(play_does_not_start_while_a_level_or_an_asset_loads)
{
	open_level();
	std::optional<std::string> blocker = "a level is loading";
	edt::play_session gated{ state, services, world_ctrl, selection, events,
		edt::play_session_options{ .start_blocker = [&blocker] { return blocker; } } };

	BOOST_TEST((gated.start_refusal() == blocker));
	BOOST_TEST(!gated.start());
	BOOST_TEST(play_set() == nullptr);
	BOOST_TEST(!state.edits_locked());
	BOOST_TEST(seen.empty());

	blocker.reset();
	BOOST_TEST(!gated.start_refusal().has_value());
	BOOST_TEST(gated.start());
	gated.stop();
}

BOOST_AUTO_TEST_CASE(stop_called_again_from_an_event_raised_during_stop_does_nothing)
{
	open_level();
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(world_ctrl.switch_to_world(1)); // Stop switches back to w0, which raises active_world_changed
	int stops_inside = 0;
	const edt::connection again = events.active_world_changed.subscribe([this, &stops_inside](edt::document_id) {
		if (play.active()) {
			++stops_inside;
			play.stop();
		}
	});

	play.stop();

	BOOST_TEST(stops_inside >= 1);
	BOOST_TEST((play.state() == edt::play_state::stopped));
	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::stopped, edt::play_reason::user } }));
	BOOST_TEST(contexts().level_set().active());
	BOOST_TEST(!state.edits_locked());
	BOOST_TEST(*state.active_world_name() == "w0");
}

BOOST_AUTO_TEST_CASE(stop_cancels_the_tasks_of_the_play_set_and_the_level_tasks_of_the_same_layer_go_on)
{
	open_level();
	BOOST_REQUIRE(play.start());
	scn::scene_layer* layer = play_set()->layer();
	BOOST_REQUIRE(layer);
	BOOST_REQUIRE(contexts().level_set().layer() == layer);
	entt::registry* twin = &play_set()->get("w0").state();
	bool play_ran = false;
	bool level_ran = false;
	auto play_task = tasks.run(ds::lane::compute, ds::task_desc{ .name = "holds the play registry", .category = "test" },
		layer->tasks(), play_set()->tasks(), [&play_ran, twin](ds::task_context&) { play_ran = twin->valid(entt::null); });
	auto level_task = tasks.run(ds::lane::compute, ds::task_desc{ .name = "a level task", .category = "test" },
		layer->tasks(), contexts().level_set().tasks(), [&level_ran](ds::task_context&) { level_ran = true; });

	play.stop();
	pump();

	BOOST_TEST(!play_ran);
	BOOST_TEST((play_task.status() == ds::task_status::cancelled));
	BOOST_TEST(level_ran);
	BOOST_TEST((level_task.status() == ds::task_status::ready));
}

BOOST_AUTO_TEST_CASE(a_module_reload_during_play_stops_it_and_starts_it_again)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(!contexts().game_layer()->descs().is_registered("play_game_v2_desc"));

	const auto report = host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::stopped, edt::play_reason::stopped_for_reload },
		{ edt::play_state::playing, edt::play_reason::module_reloaded } }));
	BOOST_TEST((play.state() == edt::play_state::playing));
	scn::context_set* set = play_set();
	BOOST_REQUIRE(set);
	BOOST_TEST(set->layer() == contexts().game_layer());
	// The old layer is freed before the new one is made, so its address may come back: the new build's type tells.
	BOOST_TEST(set->layer()->descs().is_registered("play_game_v2_desc"));
	BOOST_TEST(set->find("w0") != nullptr);
	BOOST_TEST(services.descs()->is_registered("play_game_v2_desc"));
	BOOST_TEST(!contexts().level_set().active());
	BOOST_TEST(state.edits_locked());

	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(a_module_reload_while_paused_starts_play_again_paused)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(play.pause());

	const auto report = host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST((play.state() == edt::play_state::paused));
	BOOST_TEST((seen.back() == state_change{ edt::play_state::paused, edt::play_reason::module_reloaded }));
	scn::context_set* set = play_set();
	BOOST_REQUIRE(set);
	BOOST_TEST((set->mode() == ecs::run_mode::pause));

	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(user_is_a_fresh_play_profile_while_playing)
{
	open_level();
	namespace fs = std::filesystem;
	const fs::path base = fs::temp_directory_path() / "snk_edt_play_profile_tests";
	fs::remove_all(base);
	const fs::path editor_profile = base / "user_profile";
	const fs::path play_profile = base / "play_profile";
	fs::create_directories(editor_profile);
	fs::create_directories(play_profile);
	std::ofstream(play_profile / "save.json") << "{}"; // left by the previous session
	{
		res::resource_system profile_rs(tasks, res::mount_table{ res::mount{ .protocol = res::tag::user, .roots = { editor_profile } } });
		edt::play_session session(state, services, world_ctrl, selection, events,
			{ .profile_resources = &profile_rs, .play_profile = play_profile, .editor_profile = { editor_profile } });
		const res::tag settings{ "user://settings.txt" };
		std::ofstream(editor_profile / "settings.txt") << "editor";
		auto held = profile_rs.require_sync<res::text_resource>(settings);
		pump();
		BOOST_REQUIRE(held.is_ready());

		BOOST_REQUIRE(session.start());
		BOOST_TEST(fs::is_directory(play_profile));
		BOOST_TEST(!fs::exists(play_profile / "save.json"));
		BOOST_TEST((profile_rs.mount_roots(res::tag::user) == std::vector<fs::path>{ play_profile }));
		// What the editor profile cached is not served from the play profile.
		std::ofstream(play_profile / "settings.txt") << "play";
		auto in_play = profile_rs.require_sync<res::text_resource>(settings);
		pump();
		BOOST_REQUIRE(in_play.is_ready());
		BOOST_TEST(in_play.get_sync()->str() == "play");
		BOOST_TEST(held.get_sync()->str() == "editor"); // a handle held from before keeps what it read

		session.stop();
		BOOST_TEST((profile_rs.mount_roots(res::tag::user) == std::vector<fs::path>{ editor_profile }));
	}
	fs::remove_all(base);
}

BOOST_AUTO_TEST_CASE(play_is_refused_without_a_level)
{
	BOOST_TEST(play.start_refusal().has_value());
	BOOST_TEST(!play.start());
	BOOST_TEST(play_set() == nullptr);
	BOOST_TEST(seen.empty());
}

BOOST_AUTO_TEST_CASE(a_leftover_play_set_is_not_reused)
{
	open_level();
	contexts().create_set(edt::k_play_set, nullptr, scn::set_options{ .hot_reload = false });
	BOOST_TEST(!play.start());
	BOOST_TEST(contexts().level_set().active());
	BOOST_TEST(!state.edits_locked());
	contexts().destroy_set(edt::k_play_set);
}

BOOST_AUTO_TEST_SUITE_END()

// The Play menu as agent tools (US-63-C, #491): play.* over the MCP bridge under the owner's default policy (`play`:
// allow), and app.advance only during Play.
namespace {

struct play_commands_fixture : play_fixture
{
	~play_commands_fixture()
	{
		// First: a call still waiting must not answer into a test's local after it died.
		access.confirmations().cancel_all("the test ended");
		play.stop();
	}

	// An MCP tools/call of session "agent-1"; a `busy` call is tried again by the confirmations until it gives up.
	mcp::call_result call_tool(const std::string& name, const std::string& args = "{}")
	{
		std::optional<mcp::call_result> result;
		mcp::request_context context;
		context.session_id = "agent-1";
		bridge.call_tool(context, name, json::parse(args), nullptr, [&result](mcp::call_result r) { result = std::move(r); });
		pump();
		if (!result) {
			now += edt::agent_confirmations::busy_wait + std::chrono::seconds(1);
			access.confirmations().tick();
			pump();
		}
		BOOST_REQUIRE(result.has_value());
		return std::move(*result);
	}

	static std::string text_of_error(const mcp::call_result& result)
	{
		return result.content.empty() ? std::string{} : result.content.front().text;
	}

	void run_frames(int frames)
	{
		for (int i = 0; i < frames; ++i) contexts().update(std::chrono::duration<float>(0.016f));
	}

	bool editing = false;
	std::chrono::steady_clock::time_point now{};
	edt::action_registry actions{ commands };
	std::vector<edt::registration> play_commands = edt::register_play_commands(actions,
		{ .play = [this] { return &play; }, .person_is_editing = [this] { return editing; } });
	std::vector<edt::registration> scene_commands = edt::register_scene_commands(commands,
		{ documents, selection, services, [this] { return editing; } });
	app::frame_control frame_control;
	app::frame_command_registrations core_frame_commands = app::register_frame_commands(commands, frame_control);
	edt::play_frame_commands frame_commands = edt::register_play_frame_commands(core_frame_commands, commands,
		frame_control, [this] { return play.active(); }, events);
	test_util::temp_dir dir{ "edt_play_commands" };
	edt::agent_access access{ commands, dir.path() / ".snk" / "agent_policy.json", dir.path() / "personal.json", [this] { return now; } };
	edt::mcp_bridge_host bridge{ commands, tasks, access };
};

}

BOOST_FIXTURE_TEST_SUITE(edt_play_commands_tests, play_commands_fixture)

BOOST_AUTO_TEST_CASE(an_agent_starts_pauses_steps_and_stops_play_over_mcp)
{
	open_level();
	const mcp::call_result started = call_tool("play_start");
	BOOST_REQUIRE(!started.is_error);
	BOOST_TEST(json::serialize(started.structured) == R"json({"state":"playing","frame":0,"worlds":["w0","w1"]})json");
	BOOST_TEST(play.active());

	run_frames(3);
	const mcp::call_result playing = call_tool("play_status");
	BOOST_REQUIRE(!playing.is_error);
	BOOST_TEST(json::serialize(playing.structured) == R"json({"state":"playing","frame":3,"worlds":["w0","w1"]})json");

	const mcp::call_result paused = call_tool("play_pause");
	BOOST_REQUIRE(!paused.is_error);
	BOOST_TEST((paused.structured.as_object().at("state").as_string() == "paused"));
	run_frames(2); // paused: no frame is simulated

	const mcp::call_result stepping = call_tool("play_step", R"json({"n":2})json");
	BOOST_REQUIRE(!stepping.is_error);
	BOOST_TEST((stepping.structured.as_object().at("state").as_string() == "paused"));
	run_frames(4); // the two frames of the step, then paused again
	const mcp::call_result stepped = call_tool("play_status");
	BOOST_TEST(json::serialize(stepped.structured) == R"json({"state":"paused","frame":5,"worlds":["w0","w1"]})json");

	const mcp::call_result resumed = call_tool("play_resume");
	BOOST_REQUIRE(!resumed.is_error);
	BOOST_TEST((resumed.structured.as_object().at("state").as_string() == "playing"));

	const mcp::call_result stopped = call_tool("play_stop");
	BOOST_REQUIRE(!stopped.is_error);
	BOOST_TEST(json::serialize(stopped.structured) == R"json({"state":"stopped","frame":0,"worlds":[]})json");
	BOOST_TEST(!play.active());
	BOOST_TEST(json::serialize(call_tool("play_status").structured) == R"json({"state":"stopped","frame":0,"worlds":[]})json");

	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::agent },
		{ edt::play_state::paused, edt::play_reason::agent },
		{ edt::play_state::playing, edt::play_reason::agent },
		{ edt::play_state::stopped, edt::play_reason::agent } }));
}

BOOST_AUTO_TEST_CASE(play_start_while_paused_resumes_and_a_refusal_is_failed_with_its_reason)
{
	const mcp::call_result no_level = call_tool("play_start");
	BOOST_TEST(no_level.is_error);
	BOOST_TEST(text_of_error(no_level) == "failed: no level with a world to play");

	open_level();
	BOOST_REQUIRE(!call_tool("play_start").is_error);
	const mcp::call_result again = call_tool("play_start");
	BOOST_TEST(again.is_error);
	BOOST_TEST(text_of_error(again) == "failed: Play is running already");

	BOOST_REQUIRE(play.pause());
	const mcp::call_result resumed = call_tool("play_start");
	BOOST_REQUIRE(!resumed.is_error);
	BOOST_TEST((resumed.structured.as_object().at("state").as_string() == "playing"));
}

BOOST_AUTO_TEST_CASE(play_step_while_playing_fails_pause_first_and_the_menu_step_needs_pause)
{
	open_level();
	BOOST_REQUIRE(play.start());
	BOOST_TEST(actions.is_enabled("play.step"));
	BOOST_TEST(!actions.items("Play").at(2).enabled); // Play, Pause, Step, Stop
	BOOST_TEST(actions.items("Play").at(2).id == "play.step");

	const mcp::call_result refused = call_tool("play_step");
	BOOST_TEST(refused.is_error);
	BOOST_TEST(text_of_error(refused).find("pause first") != std::string::npos);
	BOOST_TEST(text_of_error(refused).rfind("failed", 0) == 0u);
	BOOST_TEST(play.pending_steps() == 0u);
	BOOST_TEST((play.state() == edt::play_state::playing));

	BOOST_REQUIRE(play.pause());
	BOOST_TEST(actions.items("Play").at(2).enabled);
	BOOST_TEST(actions.activate("play.step")); // the menu's Step: one frame
	BOOST_TEST(play.pending_steps() == 1u);
	BOOST_TEST(call_tool("play_step", R"json({"n":601})json").is_error); // 1..600
}

BOOST_AUTO_TEST_CASE(an_agents_play_start_during_a_persons_gesture_is_busy)
{
	open_level();
	editing = true;
	const mcp::call_result busy = call_tool("play_start");
	BOOST_TEST(busy.is_error);
	BOOST_TEST(text_of_error(busy).rfind("busy", 0) == 0u);
	BOOST_TEST(!play.active());
	// The registry gives the same answer at once.
	BOOST_TEST(commands.invoke("play.start", json::object{}, cmd::origin::agent("agent-1")).code == "busy");
	// The menu's Play is greyed out meanwhile.
	BOOST_TEST(!actions.items("Play").at(0).enabled);

	editing = false;
	BOOST_TEST(!call_tool("play_start").is_error);
	// Pause, step, resume (play.start while paused too: it touches no document), stop and status need no gesture check.
	editing = true;
	BOOST_TEST(!call_tool("play_pause").is_error);
	BOOST_TEST(!call_tool("play_step").is_error);
	const mcp::call_result resumed = call_tool("play_start");
	BOOST_REQUIRE(!resumed.is_error);
	BOOST_TEST((resumed.structured.as_object().at("state").as_string() == "playing"));
	BOOST_TEST(!call_tool("play_pause").is_error);
	BOOST_TEST(!call_tool("play_resume").is_error);
	BOOST_TEST(!call_tool("play_status").is_error);
	BOOST_TEST(!call_tool("play_stop").is_error);
	// A fresh start during the gesture is busy again.
	BOOST_TEST(text_of_error(call_tool("play_start")).rfind("busy", 0) == 0u);
	BOOST_TEST(!play.active());
}

BOOST_AUTO_TEST_CASE(an_agents_scene_edit_over_mcp_during_play_is_busy)
{
	open_level();
	BOOST_REQUIRE(!call_tool("play_start").is_error);
	const mcp::call_result renamed = call_tool("scene_node_rename", R"json({"ref":{"context":"w0","path":["00000000000000a2"]},"name":"Box"})json");
	BOOST_TEST(renamed.is_error);
	BOOST_TEST(text_of_error(renamed).rfind("busy", 0) == 0u);
	BOOST_TEST(text_of_error(renamed).find("stop play first") != std::string::npos);

	BOOST_REQUIRE(!call_tool("play_stop").is_error);
	BOOST_TEST(!call_tool("scene_node_rename", R"json({"ref":{"context":"w0","path":["00000000000000a2"]},"name":"Box"})json").is_error);
}

BOOST_AUTO_TEST_CASE(app_advance_is_disabled_outside_play_and_runs_during_play)
{
	BOOST_TEST(!core_frame_commands.advance.active()); // the core's registration is gone, the play one replaced it
	BOOST_TEST(frame_commands.commands.active());
	BOOST_TEST((commands.find("app.advance")->category == cmd::agent_category::play));
	BOOST_TEST((commands.find("app.resume")->category == cmd::agent_category::play));

	const auto agent = cmd::origin::agent("agent-1");
	BOOST_TEST(commands.invoke("app.advance", json::parse(R"json({"frames":2})json"), agent).code == "disabled");
	BOOST_TEST(commands.invoke("app.resume", json::object{}, agent).code == "disabled");
	BOOST_TEST(frame_control.pending() == 0u);

	open_level();
	BOOST_REQUIRE(play.start());
	const auto advanced = commands.invoke("app.advance", json::parse(R"json({"frames":2})json"), agent);
	BOOST_TEST(advanced.ok);
	BOOST_TEST(frame_control.pending() == 2u);
	BOOST_REQUIRE(play.pause());
	BOOST_TEST(commands.invoke("app.resume", json::object{}, agent).ok); // paused is Play too

	play.stop();
	BOOST_TEST(commands.invoke("app.advance", json::parse(R"json({"frames":1})json"), agent).code == "disabled");
}

BOOST_AUTO_TEST_CASE(app_advance_hold_is_released_whenever_play_stops)
{
	open_level();
	const auto agent = cmd::origin::agent("agent-1");
	// Stopped by the agent.
	BOOST_REQUIRE(!call_tool("play_start").is_error);
	BOOST_REQUIRE(commands.invoke("app.advance", json::parse(R"json({"frames":5})json"), agent).ok);
	BOOST_TEST(frame_control.holding());
	BOOST_REQUIRE(!call_tool("play_stop").is_error);
	BOOST_TEST(!frame_control.holding());
	BOOST_TEST(frame_control.pending() == 0u);

	// Stopped by the person while paused (app.resume is disabled now: nothing else could release it).
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(play.pause());
	BOOST_REQUIRE(commands.invoke("app.advance", json::parse(R"json({"frames":3})json"), agent).ok);
	BOOST_TEST(frame_control.holding());
	play.stop();
	BOOST_TEST(!frame_control.holding());
	BOOST_TEST(commands.invoke("app.resume", json::object{}, agent).code == "disabled");

	// Stopped for a module reload and started again: the hold does not carry over.
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(commands.invoke("app.advance", json::parse(R"json({"frames":3})json"), agent).ok);
	play.before_module_swap();
	BOOST_TEST(!frame_control.holding());
	play.after_module_swap();
	BOOST_TEST(play.active());
	BOOST_TEST(!frame_control.holding());
}

BOOST_AUTO_TEST_SUITE_END()

// The frame allocator over real frames (extract and render on the null backend): Play does not leave it fuller.
namespace {

struct engine_play_fixture : test_util::null_engine_fixture
{
	engine_play_fixture() : null_engine_fixture(false)
	{
		services.ctx_manager = storage.require_shared<scn::context_manager>();
		services.base_descs = &storage.require<desc::desc_system>();
		services.base_resources = &storage.require<res::resource_system>();
		services.base_assembler = &storage.require<scn::ecs_assembler>();
		services.base_sfactory = &storage.require<ecs::system_factory>();
		world_ctrl.on_world_switched = [this](int) { events.active_world_changed.emit(state.id()); };
		// The editor's editor_redirect_cameras stage (edt_editor_init.cpp).
		storage.require<app::frame_stage_registry>().add_stage("test_redirect_cameras", app::builtin_stage::extract,
			app::stage_side::before, [this](app::stage_context& c) {
				auto& textures = c.storage.require<rnd::render_system>().get_texture_manager();
				redirector.update(c.storage.require<scn::context_manager>(), [&textures](const res::tag& tag) { textures.remove(tag); });
			});
	}

	~engine_play_fixture()
	{
		play.stop();
		storage.require<app::frame_stage_registry>().remove_stage("test_redirect_cameras");
	}

	edt::camera_redirector redirector;
	edt::editor_services services;
	edt::scene_document state;
	edt::editor_events events;
	edt::document_manager documents{ state, services, events };
	edt::selection_service selection{ events };
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	edt::play_session play{ state, services, world_ctrl, selection, events };
};

}

BOOST_FIXTURE_TEST_SUITE(edt_play_session_frame_tests, engine_play_fixture)

BOOST_AUTO_TEST_CASE(ten_cycles_over_real_frames_do_not_grow_the_frame_allocator)
{
	const res::tag level = res::tag::make("levels/smoke_level.desc");
	state.editor_tag = level;
	state.level_tag = level;
	auto handle = services.resources()->require_sync<scn::level_desc>(level);
	BOOST_REQUIRE(level_ctrl.finish_level_load(handle));
	// The level's assets finish loading over the first frames; the first cycle only warms up.
	BOOST_REQUIRE(run_frames(5) == 5);
	const std::size_t targets = redirector.tracked_count();

	std::vector<std::size_t> used_while_playing;
	std::size_t peak_after_warm_up = 0;
	for (int cycle = 0; cycle < 10; ++cycle) {
		BOOST_TEST_CONTEXT("cycle " << cycle) {
			BOOST_REQUIRE(play.start());
			BOOST_REQUIRE(run_frames(3) == 3);
			const ds::frame_allocator_stats playing = ds::frame_allocator_usage();
			BOOST_TEST(playing.used > 0u); // the last frame's packets
			BOOST_TEST(playing.overflow_blocks == 0u);
			BOOST_TEST(redirector.tracked_count() > targets); // the play cameras draw into textures of their own
			used_while_playing.push_back(playing.used);

			play.stop();
			BOOST_REQUIRE(run_frames(1) == 1);
			BOOST_TEST(redirector.tracked_count() == targets);
			if (cycle == 1) peak_after_warm_up = ds::frame_allocator_usage().peak;
		}
	}
	BOOST_TEST(used_while_playing.back() <= used_while_playing[1]);
	BOOST_TEST(ds::frame_allocator_usage().peak <= peak_after_warm_up);
	BOOST_TEST(storage.require<scn::context_manager>().find_set(edt::k_play_set) == nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// Game mode (#294): editor_mode_controller pauses Play on the way to the editor and resumes only its own pause on the
// way back; editor.mode.toggle is the command of F11 and of agents.
namespace {

struct editor_mode_fixture : play_fixture
{
	edt::editor_mode_controller modes{ play, events };
	std::vector<edt::editor_mode> modes_seen;
	edt::connection modes_connection = events.editor_mode_changed.subscribe([this](edt::editor_mode mode) {
		modes_seen.push_back(mode);
	});
};

}

BOOST_FIXTURE_TEST_SUITE(edt_editor_mode_tests, editor_mode_fixture)

BOOST_AUTO_TEST_CASE(the_game_starts_a_stopped_play_the_editor_pauses_it_and_the_game_resumes_that_pause)
{
	open_level();
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));

	BOOST_TEST(!modes.toggle().has_value());
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST((play.state() == edt::play_state::playing));

	BOOST_TEST(!modes.toggle().has_value());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST((play.state() == edt::play_state::paused));
	BOOST_TEST(modes.holds_pause());

	BOOST_TEST(!modes.toggle().has_value());
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST((play.state() == edt::play_state::playing));
	BOOST_TEST(!modes.holds_pause());

	BOOST_TEST((modes_seen == std::vector<edt::editor_mode>{ edt::editor_mode::game, edt::editor_mode::editor, edt::editor_mode::game }));
	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::paused, edt::play_reason::user },
		{ edt::play_state::playing, edt::play_reason::user } }));
	// The same mode again changes nothing.
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST(modes_seen.size() == 3u);
}

BOOST_AUTO_TEST_CASE(a_refused_start_keeps_the_editor_mode)
{
	const std::optional<std::string> refused = modes.set_mode(edt::editor_mode::game);
	BOOST_REQUIRE(refused.has_value());
	BOOST_TEST(*refused == "no level with a world to play");
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST(!play.active());
	BOOST_TEST(modes_seen.empty());
}

BOOST_AUTO_TEST_CASE(the_reason_of_the_caller_goes_to_play)
{
	open_level();
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game, edt::play_reason::agent).has_value());
	BOOST_TEST(!modes.set_mode(edt::editor_mode::editor, edt::play_reason::agent).has_value());
	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::agent },
		{ edt::play_state::paused, edt::play_reason::agent } }));
}

BOOST_AUTO_TEST_CASE(a_pause_made_by_someone_else_is_not_lifted)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE(play.pause()); // the person's Pause in game mode
	BOOST_TEST(!modes.set_mode(edt::editor_mode::editor).has_value());
	BOOST_TEST(!modes.holds_pause());
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST((play.state() == edt::play_state::paused));
}

BOOST_AUTO_TEST_CASE(a_stop_in_editor_mode_drops_the_note_and_the_game_starts_a_fresh_play)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::editor).has_value());
	BOOST_REQUIRE(modes.holds_pause());

	play.stop(edt::play_reason::agent);
	BOOST_TEST(!modes.holds_pause());
	seen.clear();
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST((play.state() == edt::play_state::playing));
	BOOST_TEST(play.frames_run() == 0u);
	// Started, not resumed: a new set "play".
	BOOST_TEST((seen == std::vector<state_change>{ { edt::play_state::playing, edt::play_reason::user } }));
	BOOST_TEST(play_set() != nullptr);
}

BOOST_AUTO_TEST_CASE(a_restart_after_a_module_reload_is_not_the_controllers_pause)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::editor).has_value());
	// The reload stops Play and starts it again paused: a Stop all the same.
	play.before_module_swap();
	play.after_module_swap();
	BOOST_REQUIRE((play.state() == edt::play_state::paused));
	BOOST_TEST(!modes.holds_pause());
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST((play.state() == edt::play_state::paused));
}

BOOST_AUTO_TEST_CASE(an_external_resume_and_pause_is_not_the_controllers_pause)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::editor).has_value());
	BOOST_REQUIRE(play.resume());
	BOOST_TEST(!modes.holds_pause());
	BOOST_REQUIRE(play.pause());
	BOOST_TEST(!modes.holds_pause());
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST((play.state() == edt::play_state::paused));
}

BOOST_AUTO_TEST_CASE(a_stop_in_game_mode_brings_the_editor_back_but_not_without_a_ui)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	play.stop(edt::play_reason::agent);
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST((modes_seen == std::vector<edt::editor_mode>{ edt::editor_mode::game, edt::editor_mode::editor }));
	BOOST_TEST(!modes.holds_pause());

	// A module reload stops Play and starts it again: the game layout stays.
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	modes_seen.clear();
	play.before_module_swap();
	play.after_module_swap();
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST(modes_seen.empty());
	BOOST_TEST((play.state() == edt::play_state::playing));
	play.stop();
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));

	// Headless: no layout to go back to.
	edt::editor_mode_controller headless{ play, events, false };
	BOOST_REQUIRE(!headless.set_mode(edt::editor_mode::game).has_value());
	play.stop();
	BOOST_TEST((headless.mode() == edt::editor_mode::game));
}

BOOST_AUTO_TEST_CASE(a_reload_whose_restart_fails_leaves_game_mode_with_a_warning)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	std::vector<std::string> warnings;
	const engine::log_sink_handle sink = engine::add_log_sink([&warnings](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn) warnings.emplace_back(message);
	});

	play.before_module_swap();
	start_blocked = "a level is loading";
	play.after_module_swap();
	modes.after_module_swap(); // what the swap gate's hook calls next
	BOOST_TEST(!play.active());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST((modes_seen.back() == edt::editor_mode::editor));
	BOOST_TEST(std::any_of(warnings.begin(), warnings.end(),
		[](const std::string& w) { return w.find("did not start again after the module reload") != std::string::npos; }));

	// A reload that restarts Play keeps game mode.
	start_blocked.reset();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	play.before_module_swap();
	play.after_module_swap();
	modes.after_module_swap();
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
}

BOOST_AUTO_TEST_CASE(a_step_in_editor_mode_keeps_the_note)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::editor).has_value());
	BOOST_REQUIRE(play.step(2));
	BOOST_TEST(modes.holds_pause());
	BOOST_TEST(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST((play.state() == edt::play_state::playing));
}

// The start level of --game (#518): the loading screen lasts from begin_startup to finish_startup, whatever the outcome.
BOOST_AUTO_TEST_CASE(without_begin_startup_nothing_is_pending)
{
	BOOST_TEST(!modes.startup_pending());
	BOOST_TEST(modes.startup_level_label().empty());
}

BOOST_AUTO_TEST_CASE(a_loaded_start_level_enters_the_game_and_ends_the_loading_screen)
{
	modes.begin_startup("res://levels/main.desc");
	BOOST_TEST(modes.startup_pending());
	BOOST_TEST(modes.startup_level_label() == "res://levels/main.desc");
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));

	open_level();
	modes.finish_startup(true, false);
	BOOST_TEST(!modes.startup_pending());
	BOOST_TEST(modes.startup_level_label().empty());
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST((play.state() == edt::play_state::playing));
}

BOOST_AUTO_TEST_CASE(a_failed_or_cancelled_load_stays_in_the_editor_with_an_error)
{
	std::vector<std::string> errors;
	const engine::log_sink_handle sink = engine::add_log_sink([&errors](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::error) errors.emplace_back(message);
	});
	modes.begin_startup("res://levels/main.desc");
	open_level();
	modes.finish_startup(false, false);
	BOOST_TEST(!modes.startup_pending());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST(!play.active());
	BOOST_TEST(modes_seen.empty());
	BOOST_TEST(std::any_of(errors.begin(), errors.end(), [](const std::string& e) {
		return e.find("game mode not entered") != std::string::npos && e.find("res://levels/main.desc") != std::string::npos;
	}));
}

BOOST_AUTO_TEST_CASE(an_editor_that_exits_after_the_level_loaded_stays_in_the_editor)
{
	modes.begin_startup("res://levels/main.desc");
	open_level();
	modes.finish_startup(true, true);
	BOOST_TEST(!modes.startup_pending());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST(!play.active());
}

BOOST_AUTO_TEST_CASE(a_refused_start_after_the_load_stays_in_the_editor_and_ends_the_loading_screen)
{
	modes.begin_startup("res://levels/main.desc");
	open_level();
	start_blocked = "an asset is being added to the scene";
	modes.finish_startup(true, false);
	BOOST_TEST(!modes.startup_pending());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST(!play.active());
	BOOST_TEST(modes_seen.empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct editor_mode_commands_fixture : play_commands_fixture
{
	edt::editor_mode_controller modes{ play, events };
	// What editor_system wires: the agent policy's mode of the play.* command the switch would run.
	std::vector<edt::registration> mode_commands = edt::register_editor_mode_commands(actions,
		{ .modes = [this] { return &modes; }, .play = [this] { return &play; }, .person_is_editing = [this] { return editing; },
			.agent_mode_of = [this](std::string_view id) {
				const cmd::command_desc* command = commands.find(id);
				return command ? access.policy().effective(*command).mode : cmd::policy_mode::deny;
			} });

	// The person's own settings may close a command for agents like the project's file.
	void personal_policy(std::string_view json_text) { dir.write("personal.json", json_text); }

	cmd::result agent_toggle(const char* args = "{}")
	{
		return commands.invoke("editor.mode.toggle", json::parse(args), cmd::origin::agent("agent-1"));
	}
};

}

BOOST_FIXTURE_TEST_SUITE(edt_editor_mode_commands_tests, editor_mode_commands_fixture)

BOOST_AUTO_TEST_CASE(editor_mode_toggle_is_a_play_tool_that_needs_a_display_with_a_strict_schema_and_f11_in_view)
{
	const cmd::command_desc* toggle = commands.find("editor.mode.toggle");
	BOOST_REQUIRE(toggle);
	BOOST_TEST((toggle->category == cmd::agent_category::play));
	BOOST_TEST(toggle->needs_display);
	BOOST_TEST(toggle->mutates);
	BOOST_TEST(toggle->expose_as_tool);
	BOOST_TEST(json::serialize(toggle->args_schema.as_object().at("properties").as_object().at("mode").as_object().at("enum")) == R"json(["editor","game"])json");
	BOOST_TEST((toggle->args_schema.as_object().at("additionalProperties") == json::value(false)));
	BOOST_TEST((toggle->result_schema.as_object().at("additionalProperties") == json::value(false)));

	const auto items = actions.items("View");
	const auto item = std::find_if(items.begin(), items.end(), [](const edt::menu_item_info& i) { return i.id == "editor.mode.toggle"; });
	BOOST_REQUIRE((item != items.end()));
	BOOST_TEST(item->shortcut_text == "F11");
	BOOST_TEST(!item->checked);

	BOOST_TEST(commands.invoke("editor.mode.toggle", json::parse(R"json({"mode":"menu"})json")).code == cmd::codes::invalid_args);
	BOOST_TEST(commands.invoke("editor.mode.toggle", json::parse(R"json({"other":1})json")).code == cmd::codes::invalid_args);

	commands.set_headless(true);
	BOOST_TEST(commands.invoke("editor.mode.toggle", json::object{}).code == cmd::codes::unavailable_headless);
	commands.set_headless(false);
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
}

BOOST_AUTO_TEST_CASE(an_agent_switches_with_and_without_the_mode_argument_over_mcp)
{
	open_level();
	const mcp::call_result game = call_tool("editor_mode_toggle", R"json({"mode":"game"})json");
	BOOST_REQUIRE(!game.is_error);
	BOOST_TEST(json::serialize(game.structured) == R"json({"mode":"game","play_state":"playing"})json");
	BOOST_TEST(actions.items("View").front().checked);

	// The mode it is in already: nothing changes.
	const mcp::call_result again = call_tool("editor_mode_toggle", R"json({"mode":"game"})json");
	BOOST_REQUIRE(!again.is_error);
	BOOST_TEST(json::serialize(again.structured) == R"json({"mode":"game","play_state":"playing"})json");

	const mcp::call_result toggled = call_tool("editor_mode_toggle");
	BOOST_REQUIRE(!toggled.is_error);
	BOOST_TEST(json::serialize(toggled.structured) == R"json({"mode":"editor","play_state":"paused"})json");

	const mcp::call_result editor = call_tool("editor_mode_toggle", R"json({"mode":"editor"})json");
	BOOST_REQUIRE(!editor.is_error);
	BOOST_TEST(json::serialize(editor.structured) == R"json({"mode":"editor","play_state":"paused"})json");

	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::agent },
		{ edt::play_state::paused, edt::play_reason::agent } }));
}

BOOST_AUTO_TEST_CASE(a_refused_start_is_failed_with_its_reason_and_a_persons_gesture_makes_an_agent_busy)
{
	const mcp::call_result refused = call_tool("editor_mode_toggle");
	BOOST_TEST(refused.is_error);
	BOOST_TEST(text_of_error(refused) == "failed: no level with a world to play");
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));

	open_level();
	editing = true;
	BOOST_TEST(commands.invoke("editor.mode.toggle", json::object{}, cmd::origin::agent("agent-1")).code == cmd::codes::busy);
	BOOST_TEST(!play.active());
	editing = false;

	// A person's F11 (the menu's activate) switches with reason user.
	BOOST_TEST(actions.activate("editor.mode.toggle"));
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST((seen.back() == state_change{ edt::play_state::playing, edt::play_reason::user }));
}

BOOST_AUTO_TEST_CASE(an_agents_switch_is_denied_where_the_policy_denies_the_play_command_it_would_run)
{
	open_level();
	personal_policy(R"json({"commands":{"play.start":"deny"}})json");
	const cmd::result start = agent_toggle(R"json({"mode":"game"})json");
	BOOST_TEST(start.code == cmd::codes::denied);
	BOOST_TEST(start.error.find("play.start") != std::string::npos);
	BOOST_TEST(!play.active());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	// A person's F11 is not an agent's call.
	BOOST_TEST(actions.activate("editor.mode.toggle"));
	BOOST_TEST((play.state() == edt::play_state::playing));

	personal_policy(R"json({ "commands": { "play.pause": "deny" } })json"); // another size: seen as changed whatever the clock
	BOOST_TEST(agent_toggle().code == cmd::codes::denied);
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
	BOOST_TEST((play.state() == edt::play_state::playing));

	personal_policy(R"json({"commands":{"play.resume":"ask"}})json");
	BOOST_REQUIRE(agent_toggle().ok); // to the editor: pauses
	BOOST_REQUIRE(modes.holds_pause());
	const cmd::result resume = agent_toggle();
	BOOST_TEST(resume.code == cmd::codes::denied);
	BOOST_TEST(resume.error.find("play.resume needs a person's approval") != std::string::npos);
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
	BOOST_TEST((play.state() == edt::play_state::paused));
	// No play.* change: the policy of play.* is not asked.
	BOOST_TEST(agent_toggle(R"json({"mode":"editor"})json").ok);
}

BOOST_AUTO_TEST_CASE(with_play_on_ask_a_persons_yes_to_the_toggle_is_enough_to_start_play)
{
	open_level();
	personal_policy(R"json({"categories":{"play":"ask"}})json");
	access.set_questions_routed(true);
	access.set_person_present(true);

	std::optional<cmd::result> outcome;
	cmd::call request;
	request.args = json::object{};
	request.from = cmd::origin::agent("agent-1");
	access.confirmations().invoke("editor.mode.toggle", std::move(request), [&outcome](cmd::result r) { outcome = std::move(r); });
	BOOST_REQUIRE(!outcome.has_value()); // the person is asked about the toggle
	const auto waiting = access.confirmations().pending();
	BOOST_REQUIRE(waiting.size() == 1u);
	BOOST_TEST(waiting.front().command == "editor.mode.toggle");
	BOOST_REQUIRE(access.confirmations().answer(waiting.front().id, edt::confirmation_answer::allow_once));
	pump();
	BOOST_REQUIRE(outcome.has_value());
	BOOST_TEST(outcome->ok, outcome->error);
	BOOST_TEST((play.state() == edt::play_state::playing));
	BOOST_TEST((modes.mode() == edt::editor_mode::game));
}

BOOST_AUTO_TEST_CASE(a_toggle_allowed_by_its_own_entry_does_not_open_a_play_command_on_ask)
{
	open_level();
	personal_policy(R"json({"categories":{"play":"ask"},"commands":{"editor.mode.toggle":"allow"}})json");
	const cmd::result start = agent_toggle(R"json({"mode":"game"})json");
	BOOST_TEST(start.code == cmd::codes::denied);
	BOOST_TEST(start.error.find("play.start needs a person's approval") != std::string::npos);
	BOOST_TEST(!play.active());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
}

BOOST_AUTO_TEST_SUITE_END()
