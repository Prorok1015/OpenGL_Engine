#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/application/application.h>
#include <snk/game/gs_engine_frame_stages.h>
#include <snk/game/gs_input_routing.h>
#include <snk/input/inp_ecs_input_manager.h>
#include <snk/input/inp_events.hpp>
#include <snk/input/inp_input_router.h>
#include <snk/scene/level/scn_context_manager.h>
#include <cmath>
#include <memory>

// Input routing (#113): the raw state of ecs_input_manager, the bindings of inp::input_router and
// the state each context finds in its ctx. Headless: events are fed straight into the manager.

namespace {

using core::inp::KEY_ACTION;
using core::inp::KEYBOARD_BUTTONS;
using core::inp::MOUSE_BUTTONS;

constexpr auto idx(KEYBOARD_BUTTONS k) { return static_cast<std::size_t>(k); }
constexpr auto idx(MOUSE_BUTTONS b) { return static_cast<std::size_t>(b); }

// Left half and right half of a 400x200 window.
const glm::ivec4 LEFT_RECT{ 0, 0, 199, 200 };
const glm::ivec4 RIGHT_RECT{ 200, 0, 400, 200 };

// What a system running inside a context saw.
struct probe
{
	int runs = 0;
	bool focus = false;
	bool key_w = false;
	bool left_button = false;
};

void probe_input_system(const inp::input_state& state, const inp::input_focus& focus, probe& out)
{
	++out.runs;
	out.focus = focus.active;
	out.key_w = state.keyboard[idx(KEYBOARD_BUTTONS::W)];
	out.left_button = state.mouse[idx(MOUSE_BUTTONS::LEFT)];
}

struct router_fixture : test_util::scene_env
{
	router_fixture()
		: source(std::make_shared<inp::ecs_input_manager>())
		, router(source)
		, hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{
		sf.register_system<probe_input_system>("test::probe_input", { .phase = ecs::phase::pre_update, .run_in_edit = true });
	}

	void move_to(glm::vec2 pos)
	{
		inp::input_event e;
		e.construct_payload<core::inp::cursor_move_event>(core::inp::cursor_move_event{ pos });
		source->on_handle_event(nullptr, e);
	}

	void key(KEYBOARD_BUTTONS k, KEY_ACTION a)
	{
		inp::input_event e;
		e.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ k, a });
		source->on_handle_event(nullptr, e);
	}

	void click(MOUSE_BUTTONS b, KEY_ACTION a)
	{
		inp::input_event e;
		e.construct_payload<core::inp::mouse_click_event>(core::inp::mouse_click_event{ b, a });
		source->on_handle_event(nullptr, e);
	}

	scn::world_context& make(std::string_view set, std::string_view name, scn::context_kind kind = scn::context_kind::level)
	{
		if (!mgr.find_set(set)) mgr.create_set(set);
		auto& context = mgr.create_context(set, name, kind, { "test::probe_input" });
		context.state().ctx().emplace<probe>();
		return context;
	}

	static const inp::input_state& state_of(scn::world_context& context) { return context.state().ctx().get<inp::input_state>(); }
	static bool focus_of(scn::world_context& context) { return context.state().ctx().get<inp::input_focus>().active; }

	std::shared_ptr<inp::ecs_input_manager> source;
	inp::input_router router;
	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::context_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(InpInputRouterTests, router_fixture)

// ---------------------------------------------------------------- router

BOOST_AUTO_TEST_CASE(Router_ContextUnderTheCursorGetsTheFullStateTheOtherGetsNeutral)
{
	router.bind({ "level", "a" }, LEFT_RECT);
	router.bind({ "level", "b" }, RIGHT_RECT);
	move_to({ 50, 50 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);
	click(MOUSE_BUTTONS::LEFT, KEY_ACTION::DOWN);

	const auto a = router.route("level", "a");
	BOOST_TEST(a.focused);
	BOOST_TEST(a.state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(a.state.mouse[idx(MOUSE_BUTTONS::LEFT)]);

	const auto b = router.route("level", "b");
	BOOST_TEST(!b.focused);
	BOOST_TEST(!b.state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(!b.state.mouse[idx(MOUSE_BUTTONS::LEFT)]);
}

BOOST_AUTO_TEST_CASE(Router_NeutralStateKeepsTheLastCursorPositionAndNoDeltas)
{
	router.bind({ "level", "a" }, LEFT_RECT);
	move_to({ 100, 50 });
	move_to({ 110, 40 });
	inp::input_event scroll;
	scroll.construct_payload<core::inp::scroll_move_event>(core::inp::scroll_move_event{ glm::vec2{ 0, 3 } });
	source->on_handle_event(nullptr, scroll);

	const auto other = router.route("level", "other");
	BOOST_TEST(!other.focused);
	BOOST_TEST((other.state.mouse_pos == glm::vec2{ 110, 40 }));
	BOOST_TEST((other.state.mouse_prev == glm::vec2{ 110, 40 }));
	BOOST_TEST((other.state.mouse_dir == glm::vec2{ 0 }));
	BOOST_TEST((other.state.scroll == glm::vec2{ 0 }));

	const auto bound = router.route("level", "a");
	BOOST_TEST((bound.state.scroll == glm::vec2{ 0, 3 }));
}

BOOST_AUTO_TEST_CASE(Router_CursorStepIsNormalizedByTheSizeOfTheBinding)
{
	router.bind({ "level", "a" }, { 0, 0, 200, 100 });
	move_to({ 100, 50 });
	move_to({ 110, 40 });

	const auto routed = router.route("level", "a");
	BOOST_TEST((routed.state.mouse_prev == glm::vec2{ 100, 50 }));
	BOOST_TEST((routed.state.mouse_pos == glm::vec2{ 110, 40 }));
	// (prev - pos) / (size / 2) = (-10, 10) / (100, 50)
	BOOST_TEST(routed.state.mouse_dir.x == -0.1f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(routed.state.mouse_dir.y == 0.2f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(Router_KeyboardAndDeltasAreIgnoredWhileTheCursorIsOutsideTheRect)
{
	router.bind({ "level", "a" }, { 100, 100, 200, 200 });
	move_to({ 10, 10 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);

	auto outside = router.route("level", "a");
	BOOST_TEST(!outside.focused);
	BOOST_TEST((outside.state.mouse_pos == glm::vec2{ 10, 10 })); // the cursor is still tracked
	BOOST_TEST(!outside.state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST((outside.state.mouse_dir == glm::vec2{ 0 }));

	move_to({ 150, 150 });
	auto inside = router.route("level", "a");
	BOOST_TEST(inside.focused);
	BOOST_TEST(inside.state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(Router_ReleaseOutsideTheRectIsNotLost)
{
	router.bind({ "level", "a" }, { 100, 100, 200, 200 });
	move_to({ 150, 150 });
	click(MOUSE_BUTTONS::LEFT, KEY_ACTION::DOWN);
	BOOST_TEST(router.route("level", "a").state.mouse[idx(MOUSE_BUTTONS::LEFT)]);

	move_to({ 5, 5 });                                  // a drag that left the rect...
	click(MOUSE_BUTTONS::LEFT, KEY_ACTION::UP);         // ...and was released there
	move_to({ 150, 150 });
	BOOST_TEST(!router.route("level", "a").state.mouse[idx(MOUSE_BUTTONS::LEFT)]);
}

BOOST_AUTO_TEST_CASE(Router_FocusFollowsTheBindingUnderTheCursor)
{
	router.bind({ "level", "a" }, LEFT_RECT);
	router.bind({ "level", "b" }, RIGHT_RECT);

	move_to({ 50, 50 });
	BOOST_TEST(router.route("level", "a").focused);
	BOOST_TEST(!router.route("level", "b").focused);

	move_to({ 300, 50 });
	BOOST_TEST(!router.route("level", "a").focused);
	BOOST_TEST(router.route("level", "b").focused);

	move_to({ 300, 500 }); // below both
	BOOST_TEST(!router.route("level", "a").focused);
	BOOST_TEST(!router.route("level", "b").focused);
}

BOOST_AUTO_TEST_CASE(Router_OverlappingRectsTheLastBoundWins)
{
	router.bind({ "level", "under" }, { 0, 0, 300, 300 });
	router.bind({ "level", "over" }, { 100, 100, 200, 200 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);

	move_to({ 150, 150 }); // in both
	BOOST_TEST(router.route("level", "over").focused);
	BOOST_TEST(!router.route("level", "under").focused);

	move_to({ 20, 20 });   // only in the lower one
	BOOST_TEST(!router.route("level", "over").focused);
	BOOST_TEST(router.route("level", "under").focused);
}

BOOST_AUTO_TEST_CASE(Router_ASetBindingFeedsEveryContextOfTheSetOnly)
{
	router.bind({ "level", "" }, LEFT_RECT);
	move_to({ 50, 50 });

	BOOST_TEST(router.route("level", "a").focused);
	BOOST_TEST(router.route("level", "b").focused);
	BOOST_TEST(!router.route("play", "a").focused);
}

BOOST_AUTO_TEST_CASE(Router_DefaultTargetCoversTheWholeBackbufferWithoutNormalizedDelta)
{
	router.set_default_target(inp::input_target{ .set = "level" });
	move_to({ 100000, 100000 });
	move_to({ 100010, 100020 });

	const auto routed = router.route("level", "anything");
	BOOST_TEST(routed.focused);
	BOOST_TEST((routed.state.mouse_dir == glm::vec2{ 0 }));
	BOOST_TEST(std::isfinite(routed.state.mouse_dir.x));
	BOOST_TEST(!router.route("play", "anything").focused);
}

BOOST_AUTO_TEST_CASE(Router_ABindingUnderTheCursorBeatsTheDefaultTarget)
{
	router.set_default_target(inp::input_target{ .set = "level" });
	router.bind({ "preview", "pv" }, LEFT_RECT);

	move_to({ 50, 50 });
	BOOST_TEST(router.route("preview", "pv").focused);
	BOOST_TEST(!router.route("level", "main").focused);

	move_to({ 300, 50 }); // outside every binding: back to the default
	BOOST_TEST(!router.route("preview", "pv").focused);
	BOOST_TEST(router.route("level", "main").focused);
}

BOOST_AUTO_TEST_CASE(Router_ClearBindingsDropsThemButKeepsTheDefault)
{
	router.set_default_target(inp::input_target{ .set = "level" });
	router.bind({ "preview", "pv" }, LEFT_RECT);
	move_to({ 50, 50 });
	BOOST_REQUIRE(router.binding_count() == 1u);

	router.clear_bindings();
	BOOST_TEST(router.binding_count() == 0u);
	BOOST_TEST(!router.route("preview", "pv").focused);
	BOOST_TEST(router.route("level", "main").focused);
}

BOOST_AUTO_TEST_CASE(Router_NoBindingNoDefaultMeansNeutralEverywhere)
{
	move_to({ 50, 50 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);
	const auto routed = router.route("level", "main");
	BOOST_TEST(!routed.focused);
	BOOST_TEST(!routed.state.keyboard[idx(KEYBOARD_BUTTONS::W)]);
}

// ---------------------------------------------------------------- contexts

BOOST_AUTO_TEST_CASE(Contexts_ViewportInputOfThePrefabDoesNotReachTheMainContext)
{
	auto& main_ctx = make("level", "main");
	auto& preview = make("level", "__prefab_preview", scn::context_kind::preview);

	router.bind({ "level", "main" }, LEFT_RECT);
	router.bind({ "level", "__prefab_preview" }, RIGHT_RECT);
	move_to({ 300, 50 }); // over the prefab viewport
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);
	click(MOUSE_BUTTONS::LEFT, KEY_ACTION::DOWN);

	gs::route_input(router, mgr);

	BOOST_TEST(focus_of(preview));
	BOOST_TEST(state_of(preview).keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(state_of(preview).mouse[idx(MOUSE_BUTTONS::LEFT)]);
	BOOST_TEST(!focus_of(main_ctx));
	BOOST_TEST(!state_of(main_ctx).keyboard[idx(KEYBOARD_BUTTONS::W)]);
	BOOST_TEST(!state_of(main_ctx).mouse[idx(MOUSE_BUTTONS::LEFT)]);

	move_to({ 50, 50 }); // and back over the main viewport
	gs::route_input(router, mgr);
	BOOST_TEST(focus_of(main_ctx));
	BOOST_TEST(!focus_of(preview));
	BOOST_TEST(!state_of(preview).keyboard[idx(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(Contexts_ANonFocusedContextGetsNeutralState)
{
	auto& focused = make("level", "a");
	auto& other = make("level", "b");
	auto& elsewhere = make("preview:p", "pv", scn::context_kind::preview);
	router.bind({ "level", "a" }, LEFT_RECT);
	move_to({ 120, 80 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);

	gs::route_input(router, mgr);

	BOOST_TEST(focus_of(focused));
	BOOST_TEST(state_of(focused).keyboard[idx(KEYBOARD_BUTTONS::W)]);
	for (auto* context : { &other, &elsewhere }) {
		BOOST_TEST(!focus_of(*context));
		BOOST_TEST(!state_of(*context).keyboard[idx(KEYBOARD_BUTTONS::W)]);
		BOOST_TEST((state_of(*context).mouse_pos == glm::vec2{ 120, 80 }));
		BOOST_TEST((state_of(*context).mouse_dir == glm::vec2{ 0 }));
	}
}

BOOST_AUTO_TEST_CASE(Contexts_WithoutTheEditorEveryContextOfTheLevelSetGetsTheState)
{
	auto& a = make("level", "a");
	auto& b = make("level", "b");
	auto& play = make("play", "a", scn::context_kind::play);
	router.set_default_target(inp::input_target{ .set = "level" });
	move_to({ 700, 400 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);

	gs::route_input(router, mgr);

	for (auto* context : { &a, &b }) {
		BOOST_TEST(focus_of(*context));
		BOOST_TEST(state_of(*context).keyboard[idx(KEYBOARD_BUTTONS::W)]);
	}
	BOOST_TEST(!focus_of(play));
	BOOST_TEST(!state_of(play).keyboard[idx(KEYBOARD_BUTTONS::W)]);
}

BOOST_AUTO_TEST_CASE(Contexts_ThePlayTwinTakesTheInputFromTheLevelContextOfTheSameName)
{
	auto& level = make("level", "w");
	auto& play = make("play", "w", scn::context_kind::play);
	router.bind({ "play", "w" }, LEFT_RECT);
	move_to({ 10, 10 });
	key(KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN);

	gs::route_input(router, mgr);

	BOOST_TEST(focus_of(play));
	BOOST_TEST(!focus_of(level));
}

// ---------------------------------------------------------------- update stage

BOOST_AUTO_TEST_CASE(UpdateStage_RoutesBeforeTheContextsRunAndRebuildsTheBindingsEveryFrame)
{
	ds::app_data_storage storage;
	auto& stages = storage.construct<app::frame_stage_registry>();
	app::register_core_frame_stages(stages);
	gs::register_scene_frame_stages(stages);
	storage.construct<app::app_loop_service_interface, app::loop_service>(stages);

	// The storage gets its own manager, source and router, as the engine registers them.
	storage.construct<inp::ecs_input_manager>();
	auto& stage_mgr = storage.construct<scn::context_manager>(*rs, *assembler, sf, hot);
	auto& stage_router = storage.construct<inp::input_router>(storage.require_shared<inp::ecs_input_manager>());

	stage_mgr.create_context("level", "a", scn::context_kind::level, { "test::probe_input" }).state().ctx().emplace<probe>();
	stage_mgr.create_context("level", "b", scn::context_kind::level, { "test::probe_input" }).state().ctx().emplace<probe>();
	auto& pa = stage_mgr.find("level", "a")->state().ctx().get<probe>();
	auto& pb = stage_mgr.find("level", "b")->state().ctx().get<probe>();

	auto& raw = storage.require<inp::ecs_input_manager>();
	inp::input_event move;
	move.construct_payload<core::inp::cursor_move_event>(core::inp::cursor_move_event{ glm::vec2{ 50, 50 } });
	raw.on_handle_event(nullptr, move);
	inp::input_event w;
	w.construct_payload<core::inp::keyboard_event>(core::inp::keyboard_event{ KEYBOARD_BUTTONS::W, KEY_ACTION::DOWN });
	raw.on_handle_event(nullptr, w);

	stage_router.bind({ "level", "a" }, LEFT_RECT);

	int frames = 0;
	stages.add_stage("test_frame_counter", app::builtin_stage::present, app::stage_side::after,
		[&frames](app::stage_context& c) {
			if (++frames >= 1) c.storage.require<app::app_loop_service_interface>().request_stop();
		});
	app::application application;
	BOOST_TEST(application.run(storage) == 0);

	BOOST_TEST(pa.runs == 1);
	BOOST_TEST(pa.focus);
	BOOST_TEST(pa.key_w);
	BOOST_TEST(pb.runs == 1);
	BOOST_TEST(!pb.focus);
	BOOST_TEST(!pb.key_w);
	BOOST_TEST(stage_router.binding_count() == 0u); // consumed: the panels bind again next frame
}

BOOST_AUTO_TEST_SUITE_END()
