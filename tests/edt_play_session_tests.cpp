#include "edt_prefab_preview_kinds.hpp"
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
#include "edt_play_preflight.h"
#include "edt_play_session.h"
#include "edt_runtime_commands.h"
#include "edt_agent_access.h"
#include "edt_agent_confirmations.h"
#include "edt_mcp_bridge_host.h"
#include "test_temp_dir.hpp"
#include "edt_scene_commands.h"
#include "edt_scene_document.h"
#include "edt_scene_editor.h"
#include "edt_world_controller.h"
#include "edt_document_cast.h"
#include <snk/application/app_frame_commands.h>
#include <snk/application/app_frame_control.h>
#include <snk/application/app_module_source.h>
#include <snk/common/mem_allocator.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/core/core_project_modules.h>
#include <snk/ecs/ecs_system.h>
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_document_manager.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/editor/edt_editor_services.h>
#include <snk/editor/edt_problem_list.h>
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
#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
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

}

// A runtime component of the module, saved by its schema across a reload (#600).
struct play_marker
{
	int value = 0;
};
BOOST_DESCRIBE_STRUCT(play_marker, (), (value))

namespace {

// What the module's observer of play_marker heard in Play: a restore must not add to it.
int g_markers_seen = 0;
// Set around Play's restore only (not the level's rebuild): Build 2's keeper cannot be made then (#601).
bool g_keeper_trap = false;
// The trap throws something that is not a std::exception.
bool g_keeper_trap_unknown = false;

// A system of the module that keeps state in its fields; Build 2's one throws while the trap is set.
template<int Build>
struct play_keeper : ecs::system_interface
{
	play_keeper()
	{
		if (Build == 2 && g_keeper_trap) {
			if (g_keeper_trap_unknown) throw 42;
			throw std::runtime_error("the keeper of build 2 cannot start");
		}
	}
	void run(entt::view<entt::get_t<play_marker>> markers) { frames += static_cast<int>(markers.size()); }
	int frames = 0;
};

void play_spin(entt::view<entt::get_t<play_marker>>) {}

// A build of the game module "play_game"; Build 2 registers one more type and no longer has play_test::spin.
template<int Build>
class play_game_build final : public gs::game_module {
public:
	std::string_view name() const override { return "play_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		layer.descs().register_component_desc<play_game_desc>("play_game_desc");
		layer.catalog().add<play_marker>("test.play_marker", desc::bind_described<play_marker>("test.play_marker", 1, desc::schema_kind::component));
		layer.blackboard_types().add<play_marker>("test.play_marker", desc::bind_described<play_marker>("test.play_marker", 1, desc::schema_kind::component));
		data.require<gs::module_context>().observe<play_marker>({ .set = std::string{ edt::k_play_set } }, scn::observed::construct,
			[](ecs::bus_context&, entt::entity) { ++g_markers_seen; });
		layer.systems().register_system<&play_keeper<Build>::run, play_keeper<Build>>("play_test::keeper", {});
		if constexpr (Build == 1) {
			layer.systems().register_system<&play_spin>("play_test::spin");
		}
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
	return SNK_MODULE_ENTRY(play_game_v1_);
}

app::modules::module_entry v2_entry()
{
	return SNK_MODULE_ENTRY(play_game_v2_);
}

constexpr std::uint64_t cam_id = 0xa1;
constexpr std::uint64_t crate_id = 0xa2;

// w0: a camera that draws to the backbuffer (redirected in the editor) and a crate; w1: empty.
constexpr const char* LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":4,"name":"w0","systems":[],"children":[
		{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":2}}},
		{"__id":"00000000000000a2","name":"Crate","components":{"stat":{"__type":"stat_desc","hp":3}}}]},
	{"__type":"world_desc","__version":4,"name":"w1","systems":[]}]})";

// The same level after a change on disk: the crate is gone and a world was added.
constexpr const char* LEVEL_CHANGED = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":4,"name":"w0","systems":[],"children":[
		{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":2}}}]},
	{"__type":"world_desc","__version":4,"name":"w1","systems":[]},
	{"__type":"world_desc","__version":4,"name":"w2","systems":[]}]})";

// LEVEL with a system that runs only in Play (not in Edit, the level's mode) and holds a resource nothing else holds.
constexpr const char* PLAY_ONLY_SYSTEM = "test::hold_play_only_resource";
constexpr const char* HOLDING_LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":4,"name":"w0","systems":["test::hold_play_only_resource"],"children":[
		{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":2}}},
		{"__id":"00000000000000a2","name":"Crate","components":{"stat":{"__type":"stat_desc","hp":3}}}]},
	{"__type":"world_desc","__version":4,"name":"w1","systems":[]}]})";

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
	{"__type":"world_desc","__version":4,"name":"w0","systems":[],"components":{"gc":{"__type":"play_game_desc","speed":3}}}]})";

// MODULE_LEVEL whose world runs the module's systems.
constexpr const char* SYSTEMS_LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":4,"name":"w0","systems":["play_test::keeper","play_test::spin"],
	 "components":{"gc":{"__type":"play_game_desc","speed":3}}}]})";

using state_change = std::pair<edt::play_state, edt::play_reason>;

const core::reload_report k_reloaded{ core::reload_status::reloaded, {} };

// A component no catalog knows: a world holding one cannot be saved.
struct unsaved_thing
{
	int value = 0;
};

// A component the catalog knows without a schema: a reload drops it and says so.
struct schemaless_thing
{
	int value = 0;
};

// Holds the data of every request while `gate` is set, until release(): a load whose raw data arrives late.
class gated_resolver : public test_util::ram_resolver
{
public:
	explicit gated_resolver(const std::string_view prt) : ram_resolver(prt) {}

	async_raw_data resolve(const res::tag& tag) const override
	{
		if (!gate) return ram_resolver::resolve(tag);
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		held.emplace_back(block, files.at(tag));
		return block;
	}

	void release()
	{
		for (auto& [block, data] : std::exchange(held, {})) block->set_ready(std::move(data));
	}

	bool gate = false;
	mutable std::vector<std::pair<std::shared_ptr<core::res::res_control_block<std::vector<std::byte>>>, std::vector<std::byte>>> held;
};

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
		gate.set_play_hooks([this] { play.before_module_swap(); }, [this](const core::reload_report& report) { play.after_module_swap(report); });
		contexts().level_set().set_mode(ecs::run_mode::edit);
		// What scene service init and editor_system register: the engine's types, and the editor's cameras, which the
		// editor puts back itself.
		auto& catalog = contexts().catalog();
		if (!catalog.find(entt::type_hash<scn::name_component>::value())) scn::register_engine_components(catalog);
		if (!catalog.find("edt.editor_camera")) {
			catalog.add_editor_only<edt::editor_camera_tag>("edt.editor_camera");
			catalog.add_editor_only<edt::prefab_camera_tag>("edt.prefab_camera");
		}
		g_markers_seen = 0;
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
	edt::document_manager documents{ state, events };
	test_util::prefab_preview_kinds prefab_kinds{ documents, services };
	edt::selection_service selection{ events };
	edt::scene_editor scene_ed{ state, services };
	edt::world_controller world_ctrl{ state, services };
	edt::level_controller level_ctrl{ state, services, scene_ed, world_ctrl };
	inp::input_router router{ std::make_shared<inp::ecs_input_manager>() };
	// What editor_system's start_blocker says (a level or an asset loading); nullopt: nothing blocks.
	std::optional<std::string> start_blocked;
	// The project a module reload writes its report into, and what it told the person.
	test_util::temp_dir report_root{ "edt_play_reload_report" };
	std::vector<std::string> notices;
	// What the check before Play finds (#597), and how often it ran.
	std::vector<edt::preflight_problem> preflight_found;
	int preflight_runs = 0;
	edt::play_session play{ state, services, world_ctrl, selection, events, edt::play_session_options{ .input = &router,
		.start_blocker = [this] { return start_blocked; }, .forget_runtime_entities = [this] { ++forgotten; },
		.each_frame = [this](const std::string& id, std::function<bool()> poll) { reload_polls.emplace_back(id, std::move(poll)); },
		.now = [this] { return clock; }, .pending_resources = [this] { return pending_resources; },
		.project_root = report_root.path(), .notify = [this](const std::string& message) { notices.push_back(message); },
		.preflight = [this] { ++preflight_runs; return preflight_found; } } };
	// What a restore waits for (the game layer's loads in flight in the editor): the tests decide when it is there.
	std::size_t pending_resources = 0;
	int forgotten = 0;
	// What editor_layer::register_implicit holds for Play (the resource wait after a restore), and the wait's clock.
	std::vector<std::pair<std::string, std::function<bool()>>> reload_polls;
	std::chrono::steady_clock::time_point clock{};

	// One UI frame of those polls: one that returns false is done.
	void run_reload_polls()
	{
		auto polls = std::move(reload_polls);
		reload_polls.clear();
		for (auto& [id, poll] : polls) {
			if (poll()) reload_polls.emplace_back(id, std::move(poll));
		}
	}

	// The loads finish and the next UI frame sees it.
	void settle()
	{
		pending_resources = 0;
		pump();
		run_reload_polls();
	}

	// A swap whose restore waits: a load is in flight when the set comes back.
	void restore_waiting()
	{
		BOOST_REQUIRE(play.capture_for_reload().empty());
		play.before_module_swap();
		pending_resources = 1;
		play.after_module_swap(k_reloaded);
		BOOST_REQUIRE(play.reloading());
	}
	json::object read_report() const
	{
		std::ifstream in(report_root.path() / ".cache" / "hot_reload" / "last_play_snapshot.json", std::ios::binary);
		const std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
		return text.empty() ? json::object{} : json::parse(text).as_object();
	}

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

BOOST_AUTO_TEST_CASE(the_play_twins_keep_the_input_stack_defaults_of_their_world_descs)
{
	open_level();
	scn::input_stack_desc modal;
	modal.block_below = true;
	state.world_descs.at(0).set_input_stack(modal);

	BOOST_REQUIRE(play.start());

	scn::context_set* set = play_set();
	BOOST_REQUIRE(set);
	BOOST_REQUIRE(set->get("w0").input_stack().has_value());
	BOOST_TEST(set->get("w0").input_stack()->block_below);
	BOOST_TEST(!set->get("w1").input_stack().has_value());
	BOOST_TEST(set->get("w0").desc_index() < set->get("w1").desc_index());
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

// #600: what the editor does (game_reloader_options::before_reload): the snapshot is taken right before the swap, and
// Play comes back in the new layer with its world, its frame and its mode, without a Stop or a Start.
BOOST_AUTO_TEST_CASE(a_module_reload_during_play_keeps_its_world)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	for (int i = 0; i < 3; ++i) contexts().update(std::chrono::duration<float>(0.016f));
	entt::registry& before = play_set()->find("w0")->state();
	before.emplace<play_marker>(before.create(), play_marker{ 7 });
	BOOST_REQUIRE(g_markers_seen == 1);
	const std::uint64_t frames = play.frames_run();

	const std::string restart = play.capture_for_reload();
	BOOST_REQUIRE_MESSAGE(restart.empty(), restart);
	const auto report = host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST((seen == std::vector<state_change>{ { edt::play_state::playing, edt::play_reason::user } }));
	BOOST_TEST((play.state() == edt::play_state::playing));
	BOOST_TEST(!play.suspended());
	BOOST_TEST(forgotten == 1);
	scn::context_set* set = play_set();
	BOOST_REQUIRE(set);
	BOOST_TEST(set->layer()->descs().is_registered("play_game_v2_desc"));
	BOOST_TEST((set->mode() == ecs::run_mode::play));
	BOOST_TEST(play.frames_run() == frames);
	entt::registry& after = set->find("w0")->state();
	const auto markers = after.view<play_marker>();
	BOOST_REQUIRE(markers.size() == 1u);
	BOOST_TEST(markers.get<play_marker>(markers.front()).value == 7);
	BOOST_TEST(g_markers_seen == 1); // the observer was muted while the marker came back
	BOOST_TEST(editor_cameras(after) == 1u);
	BOOST_TEST(!contexts().level_set().active());
	BOOST_TEST(state.edits_locked());

	// The mute ended with the restore, and the set runs on.
	after.emplace<play_marker>(after.create(), play_marker{ 8 });
	BOOST_TEST(g_markers_seen == 2);
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(play.frames_run() == frames + 1);

	play.stop();
	BOOST_TEST(contexts().level_set().active());
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(a_module_reload_while_paused_keeps_play_paused)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_REQUIRE(play.pause());
	const std::uint64_t frames = play.frames_run();

	BOOST_REQUIRE(play.capture_for_reload().empty());
	const auto report = host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST((play.state() == edt::play_state::paused));
	BOOST_TEST((seen.back() == state_change{ edt::play_state::paused, edt::play_reason::user }));
	BOOST_REQUIRE(play_set());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::pause));
	BOOST_TEST(play.frames_run() == frames);

	play.stop();
	loader.unload("play_game", root);
}

// A type no catalog knows cannot be saved: the reason names it and Play restarts (the path below).
BOOST_AUTO_TEST_CASE(a_world_holding_an_unsaveable_type_restarts_play_and_names_it)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	entt::registry& world = play_set()->find("w0")->state();
	world.emplace<unsaved_thing>(world.create());

	const std::string restart = play.capture_for_reload();
	BOOST_TEST(restart.find("unsaved_thing") != std::string::npos);
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	BOOST_TEST((seen == std::vector<state_change>{
		{ edt::play_state::playing, edt::play_reason::user },
		{ edt::play_state::stopped, edt::play_reason::stopped_for_reload },
		{ edt::play_state::playing, edt::play_reason::module_reloaded } }));

	play.stop();
	loader.unload("play_game", root);
}

// A snapshot no swap used (the build was refused before it) is dropped; a swap that lost the module stops Play.
BOOST_AUTO_TEST_CASE(a_snapshot_is_dropped_unused_and_a_lost_module_stops_play)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(play.capture_for_reload().empty());
	play.end_reload();
	settle();
	play.before_module_swap();
	BOOST_TEST(!play.suspended());
	play.after_module_swap(k_reloaded);
	BOOST_TEST((seen.back() == state_change{ edt::play_state::playing, edt::play_reason::module_reloaded }));

	BOOST_REQUIRE(play.capture_for_reload().empty());
	play.before_module_swap();
	BOOST_TEST(play.suspended());
	BOOST_TEST(!play_set());
	BOOST_TEST((play.state() == edt::play_state::playing)); // nothing announced while suspended
	play.after_module_swap(core::reload_report{ core::reload_status::lost, "the module is gone" });
	BOOST_TEST((seen.back() == state_change{ edt::play_state::stopped, edt::play_reason::stopped_for_reload }));
	BOOST_TEST(!play.suspended());
	BOOST_TEST(!play_set());
	BOOST_TEST(!state.edits_locked());
	BOOST_TEST(contexts().level_set().active());

	loader.unload("play_game", root);
}

// The seed, the time scale, the world's time and the blackboard, committed and written in the last frame, come back.
BOOST_AUTO_TEST_CASE(a_module_reload_keeps_the_seed_time_and_blackboard)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	play.set_seed(42);
	play.set_time_scale(0.5f);
	BOOST_REQUIRE(play.start());
	ecs::blackboard_writer& board = play_set()->channel().external_blackboard();
	board.set(ecs::bb_key<play_marker>{ "hud.marker" }, play_marker{ 5 });
	for (int i = 0; i < 3; ++i) contexts().update(std::chrono::duration<float>(0.016f));
	board.set(ecs::bb_key<play_marker>{ "hud.pending" }, play_marker{ 6 }); // visible from the next frame
	const double world_time = play_set()->find("w0")->state().ctx().get<scn::world_time>().seconds;

	BOOST_REQUIRE(play.capture_for_reload().empty());
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	scn::context_set* set = play_set();
	BOOST_REQUIRE(set);
	BOOST_TEST(set->seed() == 42u);
	BOOST_TEST(set->time_scale() == 0.5f);
	BOOST_TEST(set->find("w0")->state().ctx().get<scn::world_time>().seconds == world_time);
	const play_marker* committed = set->channel().reader().find(ecs::bb_key<play_marker>{ "hud.marker" });
	BOOST_REQUIRE(committed != nullptr);
	BOOST_TEST(committed->value == 5);
	BOOST_TEST(set->channel().reader().find(ecs::bb_key<play_marker>{ "hud.pending" }) == nullptr);
	contexts().update(std::chrono::duration<float>(0.016f));
	const play_marker* pending = set->channel().reader().find(ecs::bb_key<play_marker>{ "hud.pending" });
	BOOST_REQUIRE(pending != nullptr);
	BOOST_TEST(pending->value == 6);

	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(a_module_reload_during_a_step_keeps_the_frames_to_step)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(play.step(3));

	BOOST_REQUIRE(play.capture_for_reload().empty());
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	BOOST_REQUIRE(play_set());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::step));
	BOOST_TEST(play_set()->pending_steps() == 3u);
	BOOST_TEST((play.state() == edt::play_state::paused));

	play.stop();
	loader.unload("play_game", root);
}

// #625 (b): the layers the module put on the input stack of "play" and the worlds it switched off survive a reload, once each.
BOOST_AUTO_TEST_CASE(a_module_reload_keeps_the_input_layers_of_play_without_duplicates)
{
	const auto menu_layers_of_play = [this] {
		const auto layers = router.stack("play").ordered();
		return std::ranges::count_if(layers, [](const inp::stack_entry* entry) { return entry->layer.name == "menu"; });
	};
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	inp::stack_layer_desc menu;
	menu.name = "menu";
	menu.owner = inp::layer_owner::module;
	router.stack("play").put("w0", 0, menu);
	router.stack("play").set_world_active("w0", false);

	for (int round = 0; round < 2; ++round) {
		BOOST_REQUIRE(play.capture_for_reload().empty());
		host.reload("play_game", build_of(round == 0 ? v2_entry() : v1_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
		play.end_reload();
		settle();

		BOOST_REQUIRE(play_set());
		BOOST_TEST(menu_layers_of_play() == 1);
		BOOST_TEST(router.stack("play").find("w0", "menu") != nullptr);
		BOOST_TEST(!router.stack("play").world_active("w0"));
	}

	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(a_module_reload_counts_the_input_that_waited_in_the_queues_of_play)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	router.inject({ "play", "w0" }, "Jump", inp::action_edge::pressed);
	router.queue_command("play", { .kind = inp::stack_command_kind::set_world_active, .world = "w0", .active = false });

	BOOST_REQUIRE(play.capture_for_reload().empty());
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	const json::object report = read_report();
	BOOST_TEST(report.at("input_injected_dropped").to_number<int>() == 1);
	BOOST_TEST(report.at("input_commands_dropped").to_number<int>() == 1);
	BOOST_TEST(router.take_injected("play").empty());
	BOOST_TEST(router.take_commands("play").empty());

	play.stop();
	loader.unload("play_game", root);
}

// A restore that fails (here: a set "play" is already there) stops Play and gives the editor back as Stop does.
BOOST_AUTO_TEST_CASE(a_failed_restore_stops_play)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(play.capture_for_reload().empty());
	play.before_module_swap();
	BOOST_REQUIRE(play.suspended());
	contexts().create_set(edt::k_play_set, contexts().game_layer(), scn::set_options{ .hot_reload = false });

	play.after_module_swap(k_reloaded);

	BOOST_TEST((seen.back() == state_change{ edt::play_state::stopped, edt::play_reason::stopped_for_reload }));
	BOOST_TEST(!play_set());
	BOOST_TEST(!state.edits_locked());
	BOOST_TEST(contexts().level_set().active());

	loader.unload("play_game", root);
}

// #600 PR 3b (HOT_RELOAD_PLAY.md 3.1): the reload leaves what it kept and dropped in the project's .cache, and tells the
// person what Play lost.
BOOST_AUTO_TEST_CASE(a_module_reload_writes_its_report_and_tells_what_play_lost)
{
	if (!contexts().catalog().find("test.schemaless")) contexts().catalog().add<schemaless_thing>("test.schemaless", nullptr);
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	entt::registry& world = play_set()->find("w0")->state();
	world.emplace<schemaless_thing>(world.create(), schemaless_thing{ 3 });

	BOOST_REQUIRE(play.capture_for_reload().empty());
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	BOOST_TEST(play.active());
	BOOST_TEST(play.reload_number() == 1u);
	BOOST_REQUIRE(notices.size() == 1u);
	BOOST_TEST(notices.front().find("test.schemaless (1)") != std::string::npos);
	const json::object report = read_report();
	BOOST_REQUIRE(report.contains("play"));
	BOOST_TEST(report.at("play").as_string() == "kept");
	BOOST_TEST(report.at("module_reload").at("status").as_string() == "reloaded");
	BOOST_TEST(report.at("dropped_no_schema").at("test.schemaless").to_number<int>() == 1);
	BOOST_TEST(report.at("mail_dropped").is_string());
	BOOST_TEST(report.at("written_at").is_string());
	BOOST_TEST(report.at("duration_ms").is_int64());
	BOOST_REQUIRE(report.at("worlds").as_array().size() == 1u);
	const json::object& w0 = report.at("worlds").as_array()[0].as_object();
	BOOST_TEST(w0.at("name").as_string() == "w0");
	BOOST_TEST(w0.at("entities").to_number<int>() >= 2); // the world's root and the schemaless entity
	BOOST_TEST(!w0.contains("state")); // counts only, unless report_full_snapshot
	BOOST_TEST(!report.contains("blackboard"));

	// A new Start counts its reloads from zero.
	play.stop();
	BOOST_REQUIRE(play.start());
	BOOST_TEST(play.reload_number() == 0u);
	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(a_module_reload_that_restarts_or_stops_play_says_why)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	entt::registry& world = play_set()->find("w0")->state();
	world.emplace<unsaved_thing>(world.create());
	BOOST_REQUIRE(!play.capture_for_reload().empty());
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();

	BOOST_REQUIRE(notices.size() == 1u);
	BOOST_TEST(notices.back().find("started again") != std::string::npos);
	BOOST_TEST(notices.back().find("unsaved_thing") != std::string::npos);
	json::object report = read_report();
	BOOST_REQUIRE(report.contains("play"));
	BOOST_TEST(report.at("play").as_string() == "restarted");
	BOOST_TEST(report.at("not_kept_because").as_string().find("unsaved_thing") != std::string::npos);
	BOOST_TEST(!report.contains("worlds"));

	BOOST_REQUIRE(play.capture_for_reload().empty());
	play.before_module_swap();
	play.after_module_swap(core::reload_report{ core::reload_status::lost, "the module is gone" });
	BOOST_REQUIRE(notices.size() == 2u);
	BOOST_TEST(notices.back().find("Play stopped") != std::string::npos);
	report = read_report();
	BOOST_TEST(report.at("play").as_string() == "stopped");
	BOOST_TEST(report.at("module_reload").at("status").as_string() == "lost");
	BOOST_TEST(report.at("module_reload").at("message").as_string() == "the module is gone");

	loader.unload("play_game", root);
}

// #601 (HOT_RELOAD_PLAY.md 7): a system the new build no longer has is left out of the world and named; a system of the
// module that keeps state in its fields is named too (its state starts over).
BOOST_AUTO_TEST_CASE(a_system_the_new_build_lacks_and_a_system_with_fields_are_named)
{
	loader.load(build_of(v1_entry()), root);
	open_level(SYSTEMS_LEVEL);
	BOOST_REQUIRE(play.start());
	BOOST_REQUIRE(play.capture_for_reload().empty());
	host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
	play.end_reload();
	settle();

	BOOST_TEST(play.active());
	BOOST_REQUIRE(play_set());
	const auto systems = play_set()->find("w0")->systems();
	BOOST_REQUIRE(systems.has_value());
	BOOST_TEST((*systems == std::vector<std::string>{ "play_test::keeper" }), boost::test_tools::per_element());
	BOOST_REQUIRE(!notices.empty());
	BOOST_TEST(notices.back().find("no longer has: w0: play_test::spin") != std::string::npos);
	BOOST_TEST(notices.back().find("fields start again: play_test::keeper") != std::string::npos);
	const json::object report = read_report();
	BOOST_REQUIRE(report.at("dropped_systems").as_array().size() == 1u);
	BOOST_TEST(report.at("dropped_systems").as_array()[0].as_string() == "w0: play_test::spin");
	BOOST_REQUIRE(report.at("systems_restarted").as_array().size() == 1u);
	BOOST_TEST(report.at("systems_restarted").as_array()[0].as_string() == "play_test::keeper");

	play.stop();
	loader.unload("play_game", root);
}

namespace {

// A play_session that asks for a revert when a restore fails (the editor's), routed through the fixture's gate; the
// trap is set around its restore only, so Build 2's keeper fails there and not in the level.
struct reverting_session
{
	explicit reverting_session(play_fixture& fixture)
		: f(fixture)
		, play(fixture.state, fixture.services, fixture.world_ctrl, fixture.selection, fixture.events, edt::play_session_options{
			.project_root = fixture.report_root.path(),
			.notify = [this](const std::string& message) { told.push_back(message); },
			.revert_module = [this](const std::string& module) {
				++revert_requests;
				reverted_module = module;
			} })
	{
		f.gate.set_play_hooks([this] { play.before_module_swap(); }, [this](const core::reload_report& report) {
			g_keeper_trap = true;
			play.after_module_swap(report);
			g_keeper_trap = false;
		});
	}

	~reverting_session()
	{
		g_keeper_trap = false;
		g_keeper_trap_unknown = false;
		play.stop();
		f.gate.set_play_hooks([&fixture = f] { fixture.play.before_module_swap(); },
			[&fixture = f](const core::reload_report& report) { fixture.play.after_module_swap(report); });
	}

	play_fixture& f;
	std::vector<std::string> told;
	int revert_requests = 0;
	std::string reverted_module;
	edt::play_session play;
};

}

// #601 (HOT_RELOAD_PLAY.md 3.5): a build Play cannot be restored in is taken back, and Play goes on in the previous one.
BOOST_AUTO_TEST_CASE(a_build_play_cannot_be_restored_in_is_taken_back_and_play_goes_on)
{
	loader.load(build_of(v1_entry()), root);
	open_level(SYSTEMS_LEVEL);
	{
		reverting_session session(*this);
		BOOST_REQUIRE(session.play.start());
		entt::registry& world = play_set()->find("w0")->state();
		world.emplace<play_marker>(world.create(), play_marker{ 5 });

		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		BOOST_REQUIRE((host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks()).status
			== core::reload_status::reloaded));
		session.play.end_reload();

		BOOST_TEST(session.revert_requests == 1);
		BOOST_TEST(session.reverted_module == "play_game");
		BOOST_TEST(session.play.reverting());
		BOOST_TEST(session.play.reloading());
		BOOST_TEST(!play_set());
		BOOST_TEST((session.play.state() == edt::play_state::playing)); // nothing announced while the revert is due

		core::reload_report reverted;
		session.play.run_revert([&] { return reverted = host.revert("play_game", gate.hooks()); },
			[] { BOOST_FAIL("a due revert is not cancelled"); });

		BOOST_TEST((reverted.status == core::reload_status::reloaded));
		BOOST_TEST(session.revert_requests == 1);
		BOOST_TEST(!session.play.reloading());
		BOOST_TEST((session.play.state() == edt::play_state::playing));
		BOOST_TEST(!contexts().game_layer()->descs().is_registered("play_game_v2_desc"));
		BOOST_REQUIRE(play_set());
		const auto markers = play_set()->find("w0")->state().view<play_marker>();
		BOOST_REQUIRE(markers.size() == 1u);
		BOOST_TEST(markers.get<play_marker>(markers.front()).value == 5);
		BOOST_REQUIRE(!session.told.empty());
		BOOST_TEST(session.told.back().find("previous build runs again") != std::string::npos);
		BOOST_TEST(session.told.back().find("cannot start") != std::string::npos);
		const json::object report = read_report();
		BOOST_TEST(report.at("play").as_string() == "reverted");
		BOOST_TEST(report.at("restore_failed_because").as_string().find("cannot start") != std::string::npos);

		// The next reload that keeps all of Play clears the line in Problems.
		session.play.stop();
		open_level(MODULE_LEVEL);
		BOOST_REQUIRE(session.play.start());
		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		BOOST_REQUIRE((host.reload("play_game", build_of(v1_entry()), [] { return build_of(v1_entry()); }, gate.hooks()).status
			== core::reload_status::reloaded));
		session.play.end_reload();
		settle();
		BOOST_TEST(session.revert_requests == 1);
		BOOST_REQUIRE(!session.told.empty());
		BOOST_TEST(session.told.back().empty());
		BOOST_TEST(read_report().at("play").as_string() == "kept");
	}
	loader.unload("play_game", root);
}

// A restore that throws something other than a std::exception is a failed restore too.
BOOST_AUTO_TEST_CASE(a_restore_that_throws_an_unknown_type_is_taken_back_too)
{
	loader.load(build_of(v1_entry()), root);
	open_level(SYSTEMS_LEVEL);
	{
		reverting_session session(*this);
		g_keeper_trap_unknown = true;
		BOOST_REQUIRE(session.play.start());
		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
		session.play.end_reload();

		BOOST_TEST(session.revert_requests == 1);
		BOOST_TEST(session.play.restore_failure() == "an exception of an unknown type");
		session.play.end_revert(host.revert("play_game", gate.hooks()));
		BOOST_TEST((session.play.state() == edt::play_state::playing));
		BOOST_TEST(read_report().at("restore_failed_because").as_string() == "an exception of an unknown type");
	}
	loader.unload("play_game", root);
}

// A Stop while the revert is due cancels it on its frame: the new build stays, the host forgets what it could take
// back, Problems says so, and no report is written.
BOOST_AUTO_TEST_CASE(a_stop_while_the_revert_is_due_cancels_it)
{
	loader.load(build_of(v1_entry()), root);
	open_level(SYSTEMS_LEVEL);
	{
		reverting_session session(*this);
		BOOST_REQUIRE(session.play.start());
		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
		session.play.end_reload();
		BOOST_REQUIRE(session.play.reverting());

		session.play.stop();
		BOOST_TEST(!session.play.reverting());
		BOOST_TEST(!session.play.reloading());
		bool reverted = false;
		session.play.run_revert([&] {
			reverted = true;
			return host.revert("play_game", gate.hooks());
		}, [&] { host.forget_revert("play_game"); });

		BOOST_TEST(!reverted);
		BOOST_TEST((session.play.state() == edt::play_state::stopped));
		BOOST_TEST(contexts().game_layer()->descs().is_registered("play_game_v2_desc"));
		BOOST_TEST((host.revert("play_game", gate.hooks()).status == core::reload_status::refused));
		BOOST_REQUIRE(!session.told.empty());
		BOOST_TEST(session.told.back().find("cancelled by Stop") != std::string::npos);
		BOOST_TEST(!std::filesystem::exists(report_root.path() / ".cache" / "hot_reload" / "last_play_snapshot.json"));
	}
	loader.unload("play_game", root);
}

// Stop and Start before the revert ran: the new session is not taken over by the revert's answer.
BOOST_AUTO_TEST_CASE(a_play_started_again_before_the_revert_is_not_stopped_by_it)
{
	loader.load(build_of(v1_entry()), root);
	open_level(SYSTEMS_LEVEL);
	{
		reverting_session session(*this);
		BOOST_REQUIRE(session.play.start());
		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
		session.play.end_reload();
		BOOST_REQUIRE(session.play.reverting());

		session.play.stop();
		BOOST_REQUIRE(session.play.start());
		BOOST_TEST(!session.play.reverting());
		const std::size_t told = session.told.size();
		session.play.end_revert(core::reload_report{ core::reload_status::refused, "no reload of it to take back" });

		BOOST_TEST((session.play.state() == edt::play_state::playing));
		BOOST_TEST(play_set());
		BOOST_TEST(session.told.size() == told);
	}
	loader.unload("play_game", root);
}

// A revert that never swapped stops Play; and a restore that fails again in the revert's swap stops it too, with no
// second revert asked for.
BOOST_AUTO_TEST_CASE(a_revert_that_cannot_bring_play_back_stops_it_and_is_asked_for_once)
{
	loader.load(build_of(v1_entry()), root);
	open_level(SYSTEMS_LEVEL);
	{
		reverting_session session(*this);
		BOOST_REQUIRE(session.play.start());
		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		host.reload("play_game", build_of(v2_entry()), [] { return build_of(v1_entry()); }, gate.hooks());
		session.play.end_reload();
		BOOST_REQUIRE(session.revert_requests == 1);

		session.play.end_revert(core::reload_report{ core::reload_status::refused, "no reload of it to take back" });

		BOOST_TEST((session.play.state() == edt::play_state::stopped));
		BOOST_TEST(!session.play.reloading());
		BOOST_TEST(!play_set());
		BOOST_REQUIRE(!session.told.empty());
		BOOST_TEST(session.told.back().find("Play stopped") != std::string::npos);
		BOOST_TEST(session.told.back().find("no reload of it to take back") != std::string::npos);
		BOOST_TEST(read_report().at("play").as_string() == "stopped");
		BOOST_TEST(read_report().at("revert_failed_because").as_string() == "no reload of it to take back");

		// The running build is 2 now: its restore fails, a revert is asked for, and its restore fails again.
		BOOST_REQUIRE(session.play.start());
		BOOST_REQUIRE(session.play.capture_for_reload().empty());
		session.play.before_module_swap();
		g_keeper_trap = true;
		session.play.after_module_swap(k_reloaded);
		BOOST_TEST(session.revert_requests == 2);
		session.play.before_module_swap();
		session.play.after_module_swap(k_reloaded);
		g_keeper_trap = false;

		BOOST_TEST(session.revert_requests == 2);
		BOOST_TEST((session.play.state() == edt::play_state::stopped));
		BOOST_TEST(!session.play.reloading());
		BOOST_TEST(!play_set());
	}
	loader.unload("play_game", root);
}

// #600 PR 3a (HOT_RELOAD_PLAY.md 6, step 6): the restored set is neither run nor shown, and Play's commands wait, until
// what its components refer to has loaded.
BOOST_AUTO_TEST_CASE(a_restore_waits_for_its_resources_hidden_and_busy)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	restore_waiting();

	BOOST_TEST(play.reloading_resources() > 0u);
	BOOST_REQUIRE(play_set());
	BOOST_TEST(!play_set()->active());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::pause));
	BOOST_TEST(edt::view_set(services) == nullptr);
	// A step would run frames of a set that waits; a pause only sets the mode the wait gives (the editor layout's).
	BOOST_TEST(!play.step(1));
	BOOST_TEST((play.state() == edt::play_state::playing)); // nothing announced
	BOOST_TEST(reload_polls.size() == 1u);

	settle();
	BOOST_TEST(!play.reloading());
	BOOST_TEST(reload_polls.empty());
	BOOST_TEST(play_set()->active());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::play));
	BOOST_TEST(edt::view_set(services) == play_set());

	play.stop();
	loader.unload("play_game", root);
}

// A Stop during the wait ends it: the poll stops at its next frame and never reaches the set that is gone.
BOOST_AUTO_TEST_CASE(a_stop_during_the_resource_wait_ends_it)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	restore_waiting();

	play.stop();
	BOOST_TEST(!play.reloading());
	BOOST_TEST(!play_set());
	run_reload_polls();
	BOOST_TEST(reload_polls.empty());
	BOOST_TEST(!play_set());

	loader.unload("play_game", root);
}

// Past the limit (on the session's clock, not on frames) the set runs on with what has loaded.
BOOST_AUTO_TEST_CASE(a_resource_wait_past_its_limit_runs_on)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	restore_waiting();

	run_reload_polls();
	BOOST_TEST(play.reloading());
	clock += edt::play_session::k_resource_wait_limit + std::chrono::seconds(1);
	run_reload_polls();
	BOOST_TEST(!play.reloading());
	BOOST_TEST(play_set()->active());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::play));

	play.stop();
	loader.unload("play_game", root);
}

// A second reload during the wait (the editor's defer holds it; a caller without one) keeps the mode the first one
// restored, not the Pause the wait holds the set in.
BOOST_AUTO_TEST_CASE(a_second_reload_during_the_wait_keeps_the_mode_to_restore)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	contexts().update(std::chrono::duration<float>(0.016f));
	const std::uint64_t frames = play.frames_run();
	restore_waiting();
	restore_waiting();

	BOOST_TEST(reload_polls.size() == 2u); // the first wait's poll ends at its next frame
	settle();
	BOOST_TEST(!play.reloading());
	BOOST_TEST(reload_polls.empty());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::play));
	BOOST_TEST(play.frames_run() == frames);

	play.stop();
	loader.unload("play_game", root);
}

// What the restored set waits for, unless a caller says otherwise, is the game layer's own loads in flight: here a desc
// of the module whose raw data the resolver holds back.
BOOST_AUTO_TEST_CASE(a_restore_waits_for_the_loads_in_flight_of_the_game_layer)
{
	const auto stored = ram->files;
	rs->unregistrate_resolver(res::tag::memory);
	auto& gated = rs->registrate_resolver<gated_resolver>(res::tag::memory);
	gated.files = stored;
	ram = &gated;
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	std::vector<std::function<bool()>> polls;
	edt::play_session waiting{ state, services, world_ctrl, selection, events, edt::play_session_options{
		.each_frame = [&polls](const std::string&, std::function<bool()> poll) { polls.push_back(std::move(poll)); } } };
	BOOST_REQUIRE(waiting.start());
	pump();
	BOOST_REQUIRE(waiting.capture_for_reload().empty());
	waiting.before_module_swap();

	const res::tag held_tag = put("held.desc", R"json({"__type":"play_game_desc"})json");
	gated.gate = true;
	const auto held = contexts().game_layer()->resources().require<desc::desc_base>(held_tag);
	pump();
	BOOST_REQUIRE(!held.is_ready());
	BOOST_REQUIRE(contexts().game_layer()->resources().pending_loads() > 0u);
	waiting.after_module_swap(k_reloaded);

	BOOST_TEST(waiting.reloading());
	BOOST_TEST(waiting.reloading_resources() > 0u);
	BOOST_REQUIRE(polls.size() == 1u);
	BOOST_TEST(polls.front()()); // still loading: the poll stays

	gated.gate = false;
	gated.release();
	pump();
	BOOST_TEST(held.is_ready());
	BOOST_TEST(!polls.front()());
	BOOST_TEST(!waiting.reloading());
	BOOST_TEST(play_set()->active());

	waiting.stop();
	loader.unload("play_game", root);
}

// Without a snapshot (a caller that takes none, or one Play could not take) the reload stops Play and starts it again.
BOOST_AUTO_TEST_CASE(without_a_snapshot_a_module_reload_stops_play_and_starts_it_again)
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

BOOST_AUTO_TEST_CASE(without_a_snapshot_a_module_reload_while_paused_starts_play_again_paused)
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
	std::vector<edt::registration> runtime_commands = edt::register_runtime_commands(commands,
		{ .play = [this] { return &play; }, .play_set = [this] { return play_set(); } });
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

BOOST_AUTO_TEST_CASE(play_commands_answer_busy_while_a_module_reload_waits)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	restore_waiting();

	for (const char* id : { "play.pause", "play.step", "play.start" }) {
		BOOST_TEST(actions.invoke(id, json::object{}).code == "busy", id);
	}
	BOOST_TEST(!actions.items("Play").at(1).enabled); // Play, Pause, Step, Stop
	BOOST_TEST(actions.items("Play").at(3).enabled); // Stop stays
	settle();
	BOOST_TEST(actions.invoke("play.pause", json::object{}).ok);

	play.stop();
	loader.unload("play_game", root);
}

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
	play.after_module_swap(k_reloaded);
	BOOST_TEST(play.active());
	BOOST_TEST(!frame_control.holding());
}

// ---------------------------------------------------------------- runtime.query / runtime.get (#552)

namespace {

// The test environment registers no engine component: the game layer gets them the way scene service init does it for
// the editor.
std::string catalog_name_of_name_component(scn::context_manager& manager)
{
	scn::component_catalog& catalog = manager.game_layer()->catalog();
	if (!catalog.find(entt::type_hash<scn::name_component>::value())) scn::register_engine_components(catalog);
	const scn::component_entry* entry = catalog.find(entt::type_hash<scn::name_component>::value());
	return entry ? entry->name : std::string{};
}

}

BOOST_AUTO_TEST_CASE(runtime_commands_fail_outside_play_with_a_pointer_to_the_document_commands)
{
	open_level();
	const mcp::call_result query = call_tool("runtime_query", "{}");
	BOOST_TEST(query.is_error);
	BOOST_TEST(text_of_error(query).find("Play is not running") != std::string::npos);
	const mcp::call_result get = call_tool("runtime_get", R"json({"entity":0})json");
	BOOST_TEST(get.is_error);
	BOOST_TEST(text_of_error(get).find("Play is not running") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(an_agent_sees_what_play_made_by_name_and_component_and_reads_its_components_without_asking)
{
	open_level();
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	const entt::entity coin = reg.create();
	reg.emplace<scn::name_component>(coin, "Coin");
	const entt::entity clone = reg.create();
	reg.emplace<scn::name_component>(clone, "Coin clone");
	const std::string name_key = catalog_name_of_name_component(contexts());
	BOOST_REQUIRE(!name_key.empty());

	const mcp::call_result found = call_tool("runtime_query", R"json({"filter":{"name_glob":"coin*"}})json");
	BOOST_REQUIRE(!found.is_error);
	const json::object& answer = found.structured.as_object();
	BOOST_TEST(answer.at("source").as_string() == "play");
	BOOST_TEST(answer.at("world").as_string() == "w0");
	BOOST_REQUIRE_EQUAL(answer.at("items").as_array().size(), 2u);
	const json::object& first = answer.at("items").as_array().at(0).as_object();
	BOOST_TEST(first.at("name").as_string() == "Coin");
	BOOST_TEST(first.at("entity").to_number<std::uint64_t>() == static_cast<std::uint64_t>(entt::to_integral(coin)));

	const mcp::call_result with_component = call_tool("runtime_query",
		json::serialize(json::object{ { "filter", json::object{ { "has_component", json::array{ json::string(name_key) } }, { "name_glob", "clone" } } } }));
	BOOST_REQUIRE(!with_component.is_error);
	BOOST_TEST(with_component.structured.as_object().at("items").as_array().size() == 1u);

	const mcp::call_result detail = call_tool("runtime_get", json::serialize(json::object{ { "entity", static_cast<std::uint64_t>(entt::to_integral(coin)) } }));
	BOOST_REQUIRE(!detail.is_error);
	const json::object& entity = detail.structured.as_object();
	BOOST_TEST(entity.at("name").as_string() == "Coin");
	BOOST_TEST(entity.at("components").as_object().contains(name_key));
	BOOST_TEST(entity.at("parent").is_null());
	BOOST_TEST(entity.at("source").as_string() == "play");
}

BOOST_AUTO_TEST_CASE(a_reused_entity_slot_is_not_found_by_the_old_id)
{
	open_level();
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	const entt::entity old_entity = reg.create();
	reg.emplace<scn::name_component>(old_entity, "Old");
	const auto old_id = static_cast<std::uint64_t>(entt::to_integral(old_entity));
	reg.destroy(old_entity);
	const entt::entity reused = reg.create();
	reg.emplace<scn::name_component>(reused, "New");
	BOOST_REQUIRE(entt::to_entity(reused) == entt::to_entity(old_entity));

	const mcp::call_result stale = call_tool("runtime_get", json::serialize(json::object{ { "entity", old_id } }));
	BOOST_TEST(stale.is_error);
	const mcp::call_result fresh = call_tool("runtime_get", json::serialize(json::object{ { "entity", static_cast<std::uint64_t>(entt::to_integral(reused)) } }));
	BOOST_REQUIRE(!fresh.is_error);
	BOOST_TEST(fresh.structured.as_object().at("name").as_string() == "New");
}

BOOST_AUTO_TEST_CASE(a_query_page_continues_by_cursor_and_the_cursor_of_another_frame_is_stale)
{
	open_level();
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	for (int i = 0; i < 5; ++i) reg.emplace<scn::name_component>(reg.create(), "spawn");
	BOOST_REQUIRE(play.pause());

	const mcp::call_result page = call_tool("runtime_query", R"json({"filter":{"name_glob":"spawn"},"page_size":2})json");
	BOOST_REQUIRE(!page.is_error);
	BOOST_TEST(page.structured.as_object().at("items").as_array().size() == 2u);
	const std::string cursor(page.structured.as_object().at("next_cursor").as_string());

	const mcp::call_result next = call_tool("runtime_query",
		json::serialize(json::object{ { "filter", json::object{ { "name_glob", "spawn" } } }, { "page_size", 10 }, { "cursor", json::string(cursor) } }));
	BOOST_REQUIRE(!next.is_error);
	BOOST_TEST(next.structured.as_object().at("items").as_array().size() == 3u);
	BOOST_TEST(!next.structured.as_object().contains("next_cursor"));

	BOOST_REQUIRE(play.step(1));
	run_frames(1);
	const mcp::call_result stale = call_tool("runtime_query",
		json::serialize(json::object{ { "filter", json::object{ { "name_glob", "spawn" } } }, { "cursor", json::string(cursor) } }));
	BOOST_TEST(stale.is_error);
	BOOST_TEST(text_of_error(stale).find("another frame") != std::string::npos);
}

namespace {

struct opaque_marker
{
	int value = 0;
};

struct hidden_marker
{
};

// Two types of the game layer's catalog for a test: one without a schema, one the editor puts back itself.
struct marker_registration
{
	explicit marker_registration(scn::context_manager& manager)
		: catalog(manager.game_layer()->catalog())
	{
		catalog.add<opaque_marker>("test.opaque", nullptr);
		catalog.add_editor_only<hidden_marker>("test.hidden");
	}

	~marker_registration()
	{
		catalog.remove("test.opaque");
		catalog.remove("test.hidden");
	}

	scn::component_catalog& catalog;
};

std::string entity_arg(entt::entity entity)
{
	return json::serialize(json::object{ { "entity", static_cast<std::uint64_t>(entt::to_integral(entity)) } });
}

}

BOOST_AUTO_TEST_CASE(a_component_without_a_schema_is_only_named_and_a_component_over_the_cap_is_truncated)
{
	open_level();
	BOOST_REQUIRE(play.start());
	const marker_registration markers(contexts());
	const std::string name_key = catalog_name_of_name_component(contexts());
	entt::registry& reg = play_set()->get("w0").state();
	const entt::entity entity = reg.create();
	reg.emplace<scn::name_component>(entity, std::string(edt::runtime_entity_max_bytes + 1024, 'x'));
	reg.emplace<opaque_marker>(entity, 7);

	const mcp::call_result detail = call_tool("runtime_get", entity_arg(entity));
	BOOST_REQUIRE(!detail.is_error);
	const json::object& answer = detail.structured.as_object();
	BOOST_TEST(answer.at("unsaved").as_array().size() == 1u);
	BOOST_TEST(answer.at("unsaved").as_array().at(0).as_string() == "test.opaque");
	BOOST_TEST(answer.at("truncated").as_array().size() == 1u);
	BOOST_TEST(answer.at("truncated").as_array().at(0).as_string() == name_key);
	BOOST_TEST(answer.at("components").as_object().empty());
}

BOOST_AUTO_TEST_CASE(strings_of_a_component_that_are_not_utf8_reach_the_answer_cleaned)
{
	open_level();
	BOOST_REQUIRE(play.start());
	const std::string name_key = catalog_name_of_name_component(contexts());
	entt::registry& reg = play_set()->get("w0").state();
	const entt::entity entity = reg.create();
	reg.emplace<scn::name_component>(entity, std::string("bad\xff\xfename"));

	const mcp::call_result detail = call_tool("runtime_get", entity_arg(entity));
	BOOST_REQUIRE(!detail.is_error);
	const json::value& value = detail.structured.as_object().at("components").as_object().at(name_key);
	BOOST_TEST(json::serialize(value).find("bad??name") != std::string::npos);
	BOOST_TEST(detail.structured.as_object().at("name").as_string() == "bad??name");
}

BOOST_AUTO_TEST_CASE(a_query_page_stops_at_its_byte_cap_and_the_cursor_continues_from_there)
{
	open_level();
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	for (int i = 0; i < 4; ++i) reg.emplace<scn::name_component>(reg.create(), "big" + std::string(100 * 1024, 'x'));
	BOOST_REQUIRE(play.pause());

	const std::string query = R"json({"filter":{"name_glob":"big*"}})json";
	const mcp::call_result first = call_tool("runtime_query", query);
	BOOST_REQUIRE(!first.is_error);
	BOOST_TEST(first.structured.as_object().at("items").as_array().size() == 3u);
	const std::string cursor(first.structured.as_object().at("next_cursor").as_string());

	const mcp::call_result second = call_tool("runtime_query",
		json::serialize(json::object{ { "filter", json::object{ { "name_glob", "big*" } } }, { "cursor", json::string(cursor) } }));
	BOOST_REQUIRE(!second.is_error);
	BOOST_TEST(second.structured.as_object().at("items").as_array().size() == 1u);
	BOOST_TEST(!second.structured.as_object().contains("next_cursor"));
}

BOOST_AUTO_TEST_CASE(a_query_walks_one_subtree_and_the_parent_and_children_of_an_entity_are_listed)
{
	open_level();
	BOOST_REQUIRE(play.start());
	const marker_registration markers(contexts());
	entt::registry& reg = play_set()->get("w0").state();
	const entt::entity root = reg.create();
	reg.emplace<scn::name_component>(root, "Root");
	const entt::entity child = reg.create();
	reg.emplace<scn::name_component>(child, "Child");
	reg.emplace<scn::parent_component>(child, root);
	const entt::entity hidden = reg.create();
	reg.emplace<scn::name_component>(hidden, "Hidden");
	reg.emplace<scn::parent_component>(hidden, root);
	reg.emplace<hidden_marker>(hidden);
	reg.emplace<scn::children_component>(root, std::vector<entt::entity>{ child, hidden });
	const entt::entity stranger = reg.create();
	reg.emplace<scn::name_component>(stranger, "Stranger");

	const mcp::call_result under = call_tool("runtime_query",
		json::serialize(json::object{ { "filter", json::object{ { "under", static_cast<std::uint64_t>(entt::to_integral(root)) } } } }));
	BOOST_REQUIRE(!under.is_error);
	std::vector<std::string> names;
	for (const json::value& item : under.structured.as_object().at("items").as_array()) names.emplace_back(item.as_object().at("name").as_string());
	BOOST_TEST((names == std::vector<std::string>{ "Root", "Child" }));

	const mcp::call_result of_root = call_tool("runtime_get", entity_arg(root));
	BOOST_REQUIRE(!of_root.is_error);
	const json::object& root_answer = of_root.structured.as_object();
	BOOST_TEST(root_answer.at("child_count").as_int64() == 1);
	BOOST_TEST(root_answer.at("children").as_array().at(0).as_object().at("name").as_string() == "Child");
	const mcp::call_result of_child = call_tool("runtime_get", entity_arg(child));
	BOOST_REQUIRE(!of_child.is_error);
	BOOST_TEST(of_child.structured.as_object().at("parent").as_object().at("name").as_string() == "Root");

	BOOST_TEST(call_tool("runtime_get", entity_arg(hidden)).is_error);
	BOOST_TEST(call_tool("runtime_get", json::serialize(json::object{ { "entity", static_cast<std::uint64_t>(entt::to_integral(child)) },
		{ "components", json::array{ "not.a.component" } } })).is_error);
}

BOOST_AUTO_TEST_CASE(a_cursor_is_good_for_one_query_of_one_play_session_and_continues_after_its_entity_even_when_entities_come_and_go)
{
	open_level();
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	std::vector<entt::entity> made;
	for (int i = 0; i < 6; ++i) {
		made.push_back(reg.create());
		reg.emplace<scn::name_component>(made.back(), "spawn");
	}
	BOOST_REQUIRE(play.pause());
	const std::string filter = R"json("filter":{"name_glob":"spawn"})json";

	const mcp::call_result page = call_tool("runtime_query", "{" + filter + R"json(,"page_size":2})json");
	BOOST_REQUIRE(!page.is_error);
	const std::string cursor(page.structured.as_object().at("next_cursor").as_string());

	// Made and destroyed while the game is paused: no entity is skipped or repeated.
	reg.destroy(made[0]);
	reg.destroy(made[3]);
	const mcp::call_result next = call_tool("runtime_query", "{" + filter + R"json(,"page_size":10,"cursor":")json" + cursor + "\"}");
	BOOST_REQUIRE(!next.is_error);
	std::vector<std::uint64_t> seen;
	for (const json::value& item : page.structured.as_object().at("items").as_array()) seen.push_back(item.as_object().at("entity").to_number<std::uint64_t>());
	for (const json::value& item : next.structured.as_object().at("items").as_array()) seen.push_back(item.as_object().at("entity").to_number<std::uint64_t>());
	BOOST_TEST(seen.size() == 5u); // six made, one of them (made[3]) destroyed before it was reached; made[0] was already listed
	BOOST_TEST((std::ranges::adjacent_find(seen) == seen.end()));

	const mcp::call_result other_filter = call_tool("runtime_query", R"json({"filter":{"name_glob":"other"},"cursor":")json" + cursor + "\"}");
	BOOST_TEST(other_filter.is_error);
	BOOST_TEST(text_of_error(other_filter).find("another query") != std::string::npos);

	// The same cursor in the next Start: frame 0 again, but not the same session.
	play.stop();
	BOOST_REQUIRE(play.start());
	const mcp::call_result old_session = call_tool("runtime_query", "{" + filter + R"json(,"cursor":")json" + cursor + "\"}");
	BOOST_TEST(old_session.is_error);
	BOOST_TEST(text_of_error(old_session).find("now frame") != std::string::npos);
}

// #600 PR 3b: while a module reload rebuilds Play's world the runtime commands answer busy; afterwards the answers carry
// the reload's number, and a cursor of before it is stale even when the frame count is the same.
BOOST_AUTO_TEST_CASE(runtime_commands_are_busy_during_a_module_reload_and_a_cursor_of_before_it_is_stale)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	for (int i = 0; i < 3; ++i) reg.emplace<scn::name_component>(reg.create(), "spawn");
	BOOST_REQUIRE(play.pause());
	const std::string filter = R"json("filter":{"name_glob":"spawn"})json";
	const mcp::call_result page = call_tool("runtime_query", "{" + filter + R"json(,"page_size":1})json");
	BOOST_REQUIRE(!page.is_error);
	BOOST_TEST(page.structured.as_object().at("reload").to_number<int>() == 0);
	const std::string cursor(page.structured.as_object().at("next_cursor").as_string());
	const std::int64_t frame = page.structured.as_object().at("frame").to_number<std::int64_t>();

	restore_waiting();
	BOOST_TEST(commands.invoke("runtime.query", json::object{}, cmd::origin::agent("agent-1")).code == "busy");
	BOOST_TEST(commands.invoke("runtime.get", json::object{ { "entity", 0 } }, cmd::origin::agent("agent-1")).code == "busy");
	const mcp::call_result busy = call_tool("runtime_query", "{}");
	BOOST_TEST(busy.is_error);
	BOOST_TEST(text_of_error(busy).find("module reload") != std::string::npos);

	settle();
	const mcp::call_result stale = call_tool("runtime_query", "{" + filter + R"json(,"cursor":")json" + cursor + "\"}");
	BOOST_TEST(stale.is_error);
	BOOST_TEST(text_of_error(stale).find("module reload") != std::string::npos);
	const mcp::call_result fresh = call_tool("runtime_query", "{" + filter + "}");
	BOOST_REQUIRE(!fresh.is_error);
	BOOST_TEST(fresh.structured.as_object().at("reload").to_number<int>() == 1);
	BOOST_TEST(fresh.structured.as_object().at("frame").to_number<std::int64_t>() == frame);
	BOOST_TEST(fresh.structured.as_object().at("items").as_array().size() == 3u);

	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(the_runtime_commands_change_nothing_and_a_paused_game_answers_the_same_twice)
{
	open_level();
	BOOST_REQUIRE(play.start());
	entt::registry& reg = play_set()->get("w0").state();
	reg.emplace<scn::name_component>(reg.create(), "Coin");
	BOOST_REQUIRE(play.pause());
	scn::world_context& world = play_set()->get("w0");
	const scn::component_catalog& catalog = contexts().game_layer()->catalog();
	scn::snapshot_report before_report;
	const std::string before = json::serialize(scn::capture_world(world, catalog, before_report));

	const mcp::call_result first = call_tool("runtime_query", "{}");
	const mcp::call_result second = call_tool("runtime_query", "{}");
	BOOST_REQUIRE(!first.is_error);
	BOOST_TEST(json::serialize(first.structured) == json::serialize(second.structured));
	const json::array& items = first.structured.as_object().at("items").as_array();
	for (const json::value& item : items) {
		const mcp::call_result detail = call_tool("runtime_get", json::serialize(json::object{ { "entity", item.as_object().at("entity") } }));
		BOOST_REQUIRE(!detail.is_error);
	}

	scn::snapshot_report after_report;
	BOOST_TEST(json::serialize(scn::capture_world(world, catalog, after_report)) == before);
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
	edt::document_manager documents{ state, events };
	test_util::prefab_preview_kinds prefab_kinds{ documents, services };
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

// #294 during a resource wait (#600): the editor layout's pause is the one the set gets once the wait ends, and the
// game layout's resume brings it back.
BOOST_AUTO_TEST_CASE(a_switch_to_the_editor_during_a_resource_wait_pauses_play_once_it_ends)
{
	loader.load(build_of(v1_entry()), root);
	open_level(MODULE_LEVEL);
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE((play.state() == edt::play_state::playing));
	restore_waiting();

	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::editor).has_value());
	BOOST_TEST(modes.holds_pause());
	BOOST_TEST((play.state() == edt::play_state::paused));
	settle();
	BOOST_TEST(!play.reloading());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::pause));

	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_TEST((play_set()->mode() == ecs::run_mode::play));

	play.stop();
	loader.unload("play_game", root);
}

BOOST_AUTO_TEST_CASE(a_restart_after_a_module_reload_is_not_the_controllers_pause)
{
	open_level();
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::game).has_value());
	BOOST_REQUIRE(!modes.set_mode(edt::editor_mode::editor).has_value());
	// The reload stops Play and starts it again paused: a Stop all the same.
	play.before_module_swap();
	play.after_module_swap(k_reloaded);
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
	play.after_module_swap(k_reloaded);
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
	play.after_module_swap(k_reloaded);
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
	play.after_module_swap(k_reloaded);
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

BOOST_AUTO_TEST_CASE(an_agents_switch_to_the_game_is_refused_by_the_check_before_play_with_its_list)
{
	open_level();
	edt::preflight_problem broken;
	broken.item.severity = edt::problem_severity::error;
	broken.item.code = "unknown_component";
	broken.item.message = "world 'w0': broken";
	broken.target.world = "w0";
	preflight_found = { broken };

	const cmd::result refused = agent_toggle(R"({"mode":"game"})");
	BOOST_TEST(!refused.ok);
	BOOST_TEST(refused.code == cmd::codes::failed);
	BOOST_TEST(refused.error.find("check before Play") != std::string::npos);
	const json::array& problems = refused.value.as_object().at("problems").as_array();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].as_object().at("code").as_string() == "unknown_component");
	BOOST_TEST(!play.active());
	BOOST_TEST((modes.mode() == edt::editor_mode::editor));
}

BOOST_AUTO_TEST_SUITE_END()

// The check before Play (#597): what it finds in the document, without loading or writing anything.
namespace {

edt::preflight_problem a_preflight(edt::problem_severity severity, std::string code = "unknown_component")
{
	edt::preflight_problem p;
	p.item.severity = severity;
	p.item.code = std::move(code);
	p.item.message = "world 'w0': component 'stat': broken";
	p.item.notes = { "/worlds/0/children/1/components/stat" };
	p.target.world = "w0";
	p.target.path = { crate_id };
	return p;
}

struct preflight_fixture : play_fixture
{
	edt::play_preflight_services checks()
	{
		edt::play_preflight_services out;
		out.systems = &sf.registry();
		out.descs = descs.get();
		out.resources = rs.get();
		out.assembler = assembler.get();
		return out;
	}

	scn::prefab_desc::prefab_node& crate() { return state.world_descs[0].get_root().children.at(1); }
	scn::prefab_desc::prefab_node& cam() { return state.world_descs[0].get_root().children.at(0); }

	// The row at `note` (a JSON pointer); with `code`, only one of that code.
	static const edt::preflight_problem* with_note(const std::vector<edt::preflight_problem>& found, std::string_view note,
		std::string_view code = {})
	{
		const auto it = std::ranges::find_if(found, [note, code](const edt::preflight_problem& p) {
			return !p.item.notes.empty() && p.item.notes.front() == note && (code.empty() || p.item.code == code);
		});
		return it == found.end() ? nullptr : &*it;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(edt_play_preflight_tests, preflight_fixture)

BOOST_AUTO_TEST_CASE(a_clean_level_has_nothing_to_report_and_the_check_changes_nothing)
{
	open_level();
	const std::uint64_t generation = state.generation();
	const std::string before = level_json();

	BOOST_TEST(edt::run_play_preflight(state, checks()).empty());
	BOOST_TEST(state.generation() == generation);
	BOOST_TEST(!state.is_dirty());
	BOOST_TEST(level_json() == before);
}

BOOST_AUTO_TEST_CASE(a_component_type_nobody_registered_is_an_error_that_leads_to_its_node)
{
	open_level();
	scn::prefab_desc::prefab_comp_node ghost;
	ghost.type_name = "ghost_desc";
	crate().components["ghost"] = std::move(ghost);

	const auto found = edt::run_play_preflight(state, checks());
	BOOST_REQUIRE(found.size() == 1u);
	const edt::preflight_problem& row = found.front();
	BOOST_TEST((row.item.severity == edt::problem_severity::error));
	BOOST_TEST(row.item.code == "unknown_component");
	BOOST_TEST(row.item.message.find("ghost_desc") != std::string::npos);
	BOOST_TEST(row.item.message.starts_with("world 'w0': "));
	BOOST_TEST(row.item.file == "t/lv.desc");
	BOOST_REQUIRE(row.item.notes.size() == 1u);
	BOOST_TEST(row.item.notes[0] == "/worlds/0/children/1/components/ghost");
	BOOST_TEST(row.target.world == "w0");
	BOOST_TEST((row.target.path == std::vector<std::uint64_t>{ crate_id }));
	BOOST_TEST(edt::has_errors(found));
}

BOOST_AUTO_TEST_CASE(a_field_the_schema_does_not_know_is_an_error_at_its_pointer)
{
	open_level();
	cam().components.at("camera").overrides["bogus"] = 1;

	const auto found = edt::run_play_preflight(state, checks());
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST(found[0].item.code == "schema");
	BOOST_TEST((found[0].item.severity == edt::problem_severity::error));
	BOOST_TEST(found[0].item.message.find("bogus") != std::string::npos);
	BOOST_TEST(found[0].item.notes.at(0) == "/worlds/0/children/0/components/camera/bogus");
	BOOST_TEST((found[0].target.path == std::vector<std::uint64_t>{ cam_id }));
}

BOOST_AUTO_TEST_CASE(a_type_nothing_spawns_is_only_a_warning)
{
	open_level();
	descs->register_component_desc<play_game_desc>("lonely_desc");
	scn::prefab_desc::prefab_comp_node lonely;
	lonely.type_name = "lonely_desc";
	crate().components["lonely"] = std::move(lonely);

	const auto found = edt::run_play_preflight(state, checks());
	BOOST_REQUIRE(found.size() == 1u);
	BOOST_TEST(found[0].item.code == "no_spawner");
	BOOST_TEST((found[0].item.severity == edt::problem_severity::warning));
	BOOST_TEST(!edt::has_errors(found));
}

BOOST_AUTO_TEST_CASE(a_tag_field_holds_a_reference_any_other_text_only_warns_about_a_missing_file)
{
	open_level();
	const res::tag there = put("there.desc", "{}");
	scn::prefab_desc::prefab_comp_node refused_sky;
	refused_sky.type_name = "skybox_desc";
	refused_sky.overrides["material"] = "user://saves/m.desc";
	crate().components["sky"] = std::move(refused_sky);
	scn::prefab_desc::prefab_comp_node missing_sky;
	missing_sky.type_name = "skybox_desc";
	missing_sky.overrides["material"] = tag_of("gone_mat.desc").string();
	cam().components["sky"] = std::move(missing_sky);
	// stat_desc has no schema: nothing says which of its fields hold a tag.
	json::object& fields = crate().components.at("stat").overrides;
	fields["url"] = "https://example.com/page";
	fields["note"] = "user://saves/a.png";
	fields["mesh"] = tag_of("gone.desc").string();
	fields["list"] = json::array{ there.string() };
	const std::size_t live = rs->live_resources();
	const std::size_t pending = rs->pending_loads();

	const auto found = edt::run_play_preflight(state, checks());
	const auto* refused = with_note(found, "/worlds/0/children/1/components/sky/material", "reference");
	BOOST_REQUIRE(refused);
	BOOST_TEST((refused->item.severity == edt::problem_severity::error));
	const auto* missing = with_note(found, "/worlds/0/children/0/components/sky/material", "missing_resource");
	BOOST_REQUIRE(missing);
	BOOST_TEST((missing->item.severity == edt::problem_severity::error));
	const auto* loose = with_note(found, "/worlds/0/children/1/components/stat/mesh", "missing_resource");
	BOOST_REQUIRE(loose);
	BOOST_TEST((loose->item.severity == edt::problem_severity::warning));
	for (const char* quiet : { "/url", "/note", "/list/0" }) {
		BOOST_TEST(!with_note(found, std::string("/worlds/0/children/1/components/stat") + quiet), quiet);
	}
	BOOST_TEST(rs->live_resources() == live);
	BOOST_TEST(rs->pending_loads() == pending);
}

BOOST_AUTO_TEST_CASE(a_web_address_in_a_text_field_does_not_stop_an_agent)
{
	open_level();
	crate().components.at("stat").overrides["label"] = "https://example.com";
	const auto found = edt::run_play_preflight(state, checks());
	BOOST_TEST(found.empty());
	BOOST_TEST(!edt::has_errors(found));
}

BOOST_AUTO_TEST_CASE(a_parent_that_did_not_load_is_an_error)
{
	open_level();
	scn::prefab_desc::prefab_comp_node child;
	child.type_name = "stat_desc";
	child.parent_desc = rs->require<desc::desc_base>(tag_of("gone_parent.desc"));
	pump();
	BOOST_REQUIRE(child.parent_desc.has_error());
	crate().components["child"] = std::move(child);

	const auto found = edt::run_play_preflight(state, checks());
	const auto* row = with_note(found, "/worlds/0/children/1/components/child/__parent");
	BOOST_REQUIRE(row);
	BOOST_TEST(row->item.code == "missing_resource");
	BOOST_TEST((row->item.severity == edt::problem_severity::error));
}

BOOST_AUTO_TEST_CASE(a_system_nobody_registered_is_reported_for_its_world)
{
	open_level();
	state.worlds_systems_list.at(1) = { "nope::system" };

	const auto found = edt::run_play_preflight(state, checks());
	const auto* row = with_note(found, "/worlds/1/systems");
	BOOST_REQUIRE(row);
	BOOST_TEST(row->item.code == "unknown_system");
	BOOST_TEST(row->target.world == "w1");
	BOOST_TEST(row->target.path.empty());
}

BOOST_AUTO_TEST_CASE(the_source_play_shows_the_last_check_and_leads_to_its_nodes)
{
	edt::editor_events problem_events;
	edt::problem_list problems{ &problem_events };
	edt::play_preflight_publisher publisher;

	publisher.publish(problems, { a_preflight(edt::problem_severity::error) });
	const auto rows = problems.list();
	BOOST_REQUIRE(rows.size() == 1u);
	BOOST_TEST(rows[0].source == "play");
	const auto target = publisher.target_of(rows[0].item);
	BOOST_REQUIRE(target.has_value());
	BOOST_TEST((target->path == std::vector<std::uint64_t>{ crate_id }));

	const std::uint64_t generation = problems.generation();
	publisher.publish(problems, { a_preflight(edt::problem_severity::error) });
	BOOST_TEST(problems.generation() == generation); // the same rows again: nothing to redraw

	publisher.publish(problems, {});
	BOOST_TEST(problems.size() == 0u);
	BOOST_TEST(!publisher.target_of(rows[0].item).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(edt_play_preflight_gate_tests, play_fixture)

BOOST_AUTO_TEST_CASE(an_error_stops_a_person_and_an_agent_until_a_person_allows_it_once)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error) };

	BOOST_TEST(!play.start(edt::play_reason::user));
	BOOST_TEST(!play.start(edt::play_reason::agent));
	BOOST_TEST(play.last_check().size() == 1u);
	BOOST_TEST(play_set() == nullptr);
	BOOST_TEST(!state.edits_locked());

	play.allow_problems_once();
	BOOST_TEST(!play.start(edt::play_reason::agent)); // never an agent's way around it
	BOOST_TEST(!play.problems_allowed());

	play.allow_problems_once();
	BOOST_TEST(play.start(edt::play_reason::user));
	BOOST_TEST(!play.problems_allowed());
	play.stop();

	BOOST_TEST(!play.start(edt::play_reason::user)); // once: the next start is checked again
}

BOOST_AUTO_TEST_CASE(warnings_do_not_stop_play)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::warning, "no_spawner") };
	BOOST_TEST(play.start(edt::play_reason::agent));
	BOOST_TEST(preflight_runs == 1);
}

BOOST_AUTO_TEST_CASE(the_restart_after_a_module_reload_is_not_checked)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error) };
	BOOST_TEST(play.start(edt::play_reason::module_reloaded));
	BOOST_TEST(preflight_runs == 0);
	BOOST_TEST(play.last_check().empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {

struct preflight_commands_fixture : play_commands_fixture
{
	preflight_commands_fixture()
	{
		dialogs.set_registrar([this](std::string_view, std::function<bool()> draw) { dialog_frames.push_back(std::move(draw)); });
	}

	~preflight_commands_fixture() { person_commands.clear(); }

	void run_dialog_frames()
	{
		for (int i = 0; i < 4 && !dialog_frames.empty(); ++i) {
			auto frames = std::move(dialog_frames);
			dialog_frames.clear();
			for (auto& frame : frames) {
				if (frame()) dialog_frames.push_back(std::move(frame));
			}
		}
	}

	edt::dialog_service::dialog_id the_question() const
	{
		const auto waiting = dialogs.list();
		BOOST_REQUIRE(waiting.size() == 1u);
		BOOST_REQUIRE(waiting[0].kind == "choice");
		return waiting[0].id;
	}

	edt::dialog_service dialogs;
	std::vector<std::function<bool()>> dialog_frames;
	int problems_shown = 0;
	// The menu of a person: what editor_system registers, with the question.
	edt::action_registry person_actions;
	std::vector<edt::registration> person_commands = edt::register_play_commands(person_actions,
		{ .play = [this] { return &play; }, .person_is_editing = [this] { return editing; }, .dialogs = &dialogs,
			.show_problems = [this] { ++problems_shown; } });
};

}

BOOST_FIXTURE_TEST_SUITE(edt_play_preflight_commands_tests, preflight_commands_fixture)

BOOST_AUTO_TEST_CASE(an_agent_is_refused_with_the_list_of_problems_and_play_does_not_start)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error), a_preflight(edt::problem_severity::warning, "no_spawner") };
	std::vector<std::string> warnings;
	const auto sink = engine::add_log_sink([&warnings](std::string_view category, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn && category == "play") warnings.emplace_back(message);
	});

	const edt::action_result refused = actions.invoke("play.start", json::object{}, cmd::origin::agent("agent-1"));
	BOOST_TEST(!refused.ok);
	BOOST_TEST(refused.code == cmd::codes::failed);
	const json::array& problems = refused.value.as_object().at("problems").as_array();
	BOOST_REQUIRE(problems.size() == 2u);
	BOOST_TEST(json::serialize(problems[0]) == R"json({"severity":"error","code":"unknown_component","message":"world 'w0': component 'stat': broken","world":"w0","path":["00000000000000a2"],"json_path":"/worlds/0/children/1/components/stat"})json");
	BOOST_TEST(problems[1].as_object().at("severity").as_string() == "warning");
	BOOST_TEST(!play.active());
	BOOST_TEST(preflight_runs == 1); // one check per start
	BOOST_REQUIRE(warnings.size() == 1u);
	BOOST_TEST(warnings[0].find("(agent)") != std::string::npos);
	BOOST_TEST(warnings[0].find("1 error(s)") != std::string::npos);

	BOOST_TEST(call_tool("play_start").is_error);
	BOOST_TEST(!play.active());

	play.allow_problems_once();
	BOOST_TEST(actions.invoke("play.start", json::object{}, cmd::origin::agent("agent-1")).code == cmd::codes::failed);
	BOOST_TEST(!play.active());
	BOOST_TEST(!play.problems_allowed());
}

BOOST_AUTO_TEST_CASE(an_answer_after_the_commands_are_gone_does_nothing)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error) };

	BOOST_REQUIRE(person_actions.activate("play.start"));
	const auto question = the_question();
	person_commands.clear();

	BOOST_TEST(dialogs.answer(question, json::parse(R"({"index":1})")).ok);
	BOOST_TEST(problems_shown == 0);
	BOOST_TEST(!play.active());
	BOOST_TEST(!play.problems_allowed());
}

BOOST_AUTO_TEST_CASE(a_persons_call_needs_an_answer_and_play_anyway_starts_it_once)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error) };

	BOOST_TEST(person_actions.invoke("play.start", json::object{}).code == cmd::codes::needs_input);
	BOOST_REQUIRE(person_actions.activate("play.start"));
	BOOST_TEST(!play.active());
	const auto question = the_question();
	BOOST_TEST(dialogs.list()[0].text.find("world 'w0': component 'stat': broken") != std::string::npos);

	BOOST_TEST(dialogs.answer(question, json::parse(R"({"index":0})")).ok);
	BOOST_TEST(play.active());
	BOOST_TEST(!play.problems_allowed());
	BOOST_TEST(problems_shown == 0);
}

BOOST_AUTO_TEST_CASE(cancel_keeps_play_off_and_show_problems_opens_them)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error) };

	BOOST_REQUIRE(person_actions.activate("play.start"));
	BOOST_TEST(dialogs.answer(the_question(), json::parse(R"({"index":1})")).ok);
	BOOST_TEST(problems_shown == 1);
	BOOST_TEST(!play.active());

	BOOST_REQUIRE(person_actions.activate("play.start"));
	BOOST_TEST(dialogs.answer(the_question(), json::parse(R"({"index":2})")).ok);
	BOOST_TEST(!play.active());
	BOOST_TEST(!play.problems_allowed());
}

BOOST_AUTO_TEST_CASE(headless_nobody_answers_play_stays_off_and_the_log_says_why)
{
	open_level();
	preflight_found = { a_preflight(edt::problem_severity::error) };
	std::vector<std::string> warnings;
	const auto sink = engine::add_log_sink([&warnings](std::string_view category, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn && category == "play") warnings.emplace_back(message);
	});
	edt::dialog_auto_answer nobody;
	nobody.cancel_all = true;
	nobody.warn_headless = true;
	dialogs.set_auto_answer(nobody);

	BOOST_REQUIRE(person_actions.activate("play.start"));
	run_dialog_frames();

	BOOST_TEST(dialogs.pending() == 0u);
	BOOST_TEST(!play.active());
	BOOST_REQUIRE(warnings.size() == 1u);
	BOOST_TEST(warnings[0].find("nobody allowed it") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_refusal_of_another_kind_is_said_not_greyed)
{
	open_level();
	start_blocked = "a level is loading";

	BOOST_TEST(person_actions.items("Play").at(0).enabled);
	BOOST_REQUIRE(person_actions.activate("play.start"));
	const auto waiting = dialogs.list();
	BOOST_REQUIRE(waiting.size() == 1u);
	BOOST_TEST(waiting[0].kind == "message");
	BOOST_TEST(waiting[0].text.find("a level is loading") != std::string::npos);
	BOOST_TEST(!play.active());
	BOOST_TEST(preflight_runs == 0);
}

BOOST_AUTO_TEST_SUITE_END()
