#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_system.h>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_screen_anchor.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_screen_anchor_desc.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// Markers over objects (WORLD_MESSAGING 3, W5 #273): a UI element names a node of a context of its own set; the
// render extractor resolves it every frame, after the updates, through the camera of the target's context. The
// projection, the choice of the camera, the scoping to the set and the "no lag" rule are checked here without a
// renderer: the extractor calls scn::resolve_screen_anchors with the size of the backbuffer, so do the tests.

namespace {

constexpr auto one_frame = std::chrono::duration<float>(0.016f);
const glm::ivec2 screen{ 800, 600 };

bool close_to(float a, float b, float eps = 0.05f) { return std::abs(a - b) <= eps; }
bool close_to(glm::vec2 a, glm::vec2 b, float eps = 0.05f) { return close_to(a.x, b.x, eps) && close_to(a.y, b.y, eps); }

// A node with an address and a place in the world.
entt::entity add_node(entt::registry& reg, std::vector<uint64_t> path, glm::vec3 position)
{
	scn::node_index::get(reg); // the hooks must exist before the first node_id_component
	const auto ent = reg.create();
	reg.emplace<scn::node_id_component>(ent, scn::node_id_component{ std::move(path) });
	reg.emplace<scn::world_transform>(ent, scn::world_transform{ glm::translate(glm::mat4{ 1.f }, position) });
	return ent;
}

// A backbuffer camera (no viewport of its own): it renders into the whole backbuffer. Looks along -z.
entt::entity add_camera(entt::registry& reg, glm::vec3 position = {}, scn::camera_component camera = {})
{
	const auto ent = reg.create();
	reg.emplace<scn::camera_component>(ent, std::move(camera));
	reg.emplace<scn::world_transform>(ent, scn::world_transform{ glm::translate(glm::mat4{ 1.f }, position) });
	return ent;
}

entt::entity add_marker(entt::registry& reg, std::string context, std::vector<uint64_t> path, glm::vec3 offset = {})
{
	const auto ent = reg.create();
	reg.emplace<scn::screen_anchor_component>(ent, scn::screen_anchor_component{ ecs::entity_ref{ std::move(context), std::move(path) }, offset });
	return ent;
}

scn::screen_anchor_result result_for(entt::registry& reg, entt::entity marker)
{
	const auto* result = reg.try_get<scn::screen_anchor_result>(marker);
	BOOST_REQUIRE_MESSAGE(result, "the marker has no result");
	return *result;
}

// The target moves by `step` along x every frame, in the update phase.
struct mover
{
	entt::entity target = entt::null;
	float step = 0.f;
};

void sys_move_target(entt::registry& reg)
{
	const auto& m = reg.ctx().get<mover>();
	reg.get<scn::world_transform>(m.target).world[3].x += m.step;
}

struct anchor_fixture : test_util::scene_env
{
	anchor_fixture()
		: mgr(*rs, *assembler, sf)
	{
		sf.register_system<&sys_move_target>("move_target", { .phase = ecs::phase::update });
	}

	scn::world_context& add_context(std::string_view set, std::string_view name, std::vector<std::string> systems = {})
	{
		return mgr.create_context(set, name, scn::context_kind::runtime, systems);
	}

	// One 3D world with a camera at the origin and a target at `target_position`, and a UI world with a marker.
	struct scene
	{
		entt::registry* world = nullptr;
		entt::registry* ui = nullptr;
		entt::entity camera = entt::null;
		entt::entity target = entt::null;
		entt::entity marker = entt::null;
	};

	scene make_scene(glm::vec3 target_position, glm::vec3 offset = {}, scn::camera_component camera = {})
	{
		scene s;
		s.world = &add_context("level", "world").state();
		s.ui = &add_context("level", "ui").state();
		s.camera = add_camera(*s.world, {}, std::move(camera));
		s.target = add_node(*s.world, { 7 }, target_position);
		s.marker = add_marker(*s.ui, "world", { 7 }, offset);
		return s;
	}

	scn::screen_anchor_result resolve(const scene& s, glm::ivec2 size = screen)
	{
		scn::resolve_screen_anchors(mgr, size);
		return result_for(*s.ui, s.marker);
	}

	ecs::system_factory sf;
	scn::context_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnScreenAnchorTests, anchor_fixture)

// ---------------------------------------------------------------- perspective

BOOST_AUTO_TEST_CASE(Perspective_AnObjectStraightAheadIsInTheMiddleOfTheScreen)
{
	const auto s = make_scene({ 0, 0, -5 });
	const auto r = resolve(s);
	BOOST_TEST(r.resolved);
	BOOST_TEST(!r.off_screen);
	BOOST_TEST(!r.behind_camera);
	BOOST_TEST(close_to(r.position, { 400, 300 }));
	BOOST_TEST(close_to(r.normalized, { 0.5f, 0.5f }, 0.001f));
}

BOOST_AUTO_TEST_CASE(Perspective_TheOffsetIsInWorldUnitsAndUpIsUpOnTheScreen)
{
	// fov 90: one unit up at distance 5 is 0.2 of the half height above the middle.
	const auto s = make_scene({ 0, 0, -5 }, { 0, 1, 0 });
	const auto r = resolve(s);
	BOOST_TEST(close_to(r.position, { 400, 240 }));
	BOOST_TEST(!r.off_screen);
}

BOOST_AUTO_TEST_CASE(Perspective_TheShiftOfAnOffsetShrinksWithDistance)
{
	const auto near_target = make_scene({ 0, 0, -5 }, { 0, 1, 0 });
	const auto at_five = resolve(near_target).position.y;
	// the same marker over a target twice as far gets half the shift
	(*near_target.world).get<scn::world_transform>(near_target.target).world[3].z = -10.f;
	const auto at_ten = resolve(near_target).position.y;
	BOOST_TEST(close_to(300.f - at_five, 60.f));
	BOOST_TEST(close_to(300.f - at_ten, 30.f));
}

BOOST_AUTO_TEST_CASE(Perspective_TheSizeOfTheViewportSetsTheAspect)
{
	// 2.5 to the right at distance 5: ndc.x = 0.5 / aspect.
	const auto s = make_scene({ 2.5f, 0, -5 });
	BOOST_TEST(close_to(resolve(s, { 800, 600 }).position, { 550, 300 }));
	BOOST_TEST(close_to(resolve(s, { 600, 600 }).position, { 450, 300 }));
	BOOST_TEST(close_to(resolve(s, { 1600, 900 }).normalized, { 0.5f + 0.5f * 0.5f / (16.f / 9.f) * 1.f, 0.5f }, 0.001f));
}

BOOST_AUTO_TEST_CASE(Perspective_ACameraWithItsOwnViewportMeasuresInsideIt)
{
	scn::camera_component camera;
	camera.m_viewport = scn::viewport{ glm::ivec4{ 100, 50, 400, 300 } };
	const auto s = make_scene({ 0, 0, -5 }, {}, camera);
	const auto r = resolve(s, { 1920, 1080 }); // the backbuffer does not matter to a camera with a viewport
	BOOST_TEST(close_to(r.position, { 200, 150 })); // from the corner of the viewport, not of the screen
	BOOST_TEST(close_to(r.normalized, { 0.5f, 0.5f }, 0.001f));
}

BOOST_AUTO_TEST_CASE(Perspective_BehindTheCameraIsFlaggedAndHasNoPosition)
{
	const auto s = make_scene({ 0, 0, 5 });
	const auto r = resolve(s);
	BOOST_TEST(r.resolved);
	BOOST_TEST(r.behind_camera);
	BOOST_TEST(r.off_screen);
	BOOST_TEST(close_to(r.position, { 0, 0 }, 0.0001f));
}

BOOST_AUTO_TEST_CASE(Perspective_OutsideTheViewportOrBeyondTheFarPlaneIsOffScreenButNotBehind)
{
	const auto side = make_scene({ 50, 0, -5 });
	auto r = resolve(side);
	BOOST_TEST(r.resolved);
	BOOST_TEST(r.off_screen);
	BOOST_TEST(!r.behind_camera);
	BOOST_TEST(r.normalized.x > 1.f);

	(*side.world).get<scn::world_transform>(side.target).world = glm::translate(glm::mat4{ 1.f }, glm::vec3{ 0, 0, -2000 });
	r = resolve(side);
	BOOST_TEST(r.off_screen);
	BOOST_TEST(!r.behind_camera);
}

BOOST_AUTO_TEST_CASE(Perspective_TheViewComesFromTheTransformOfTheCamera)
{
	const auto s = make_scene({ 2.5f, 0, -5 });
	// moved with the target: dead ahead again
	(*s.world).get<scn::world_transform>(s.camera).world = glm::translate(glm::mat4{ 1.f }, glm::vec3{ 2.5f, 0, 0 });
	BOOST_TEST(close_to(resolve(s).position, { 400, 300 }));

	// turned a quarter to the left: -z becomes -x
	(*s.world).get<scn::world_transform>(s.target).world = glm::translate(glm::mat4{ 1.f }, glm::vec3{ -5, 0, 0 });
	(*s.world).get<scn::world_transform>(s.camera).world = glm::rotate(glm::mat4{ 1.f }, glm::half_pi<float>(), glm::vec3{ 0, 1, 0 });
	BOOST_TEST(close_to(resolve(s).position, { 400, 300 }));
}

// ---------------------------------------------------------------- orthographic

BOOST_AUTO_TEST_CASE(Ortho_TheOrthoSizeIsTheVisibleHeightAndDistanceDoesNotMatter)
{
	scn::camera_component camera;
	camera.projection = scn::camera_projection::ortho;
	camera.ortho_size = 10.f; // 10 units tall, 13.33 wide at 800x600
	const auto s = make_scene({ 10.f / 3.f, 2.5f, -1 }, {}, camera);
	BOOST_TEST(close_to(resolve(s).position, { 600, 150 }));

	(*s.world).get<scn::world_transform>(s.target).world[3].z = -100.f;
	const auto far_away = resolve(s);
	BOOST_TEST(close_to(far_away.position, { 600, 150 }));
	BOOST_TEST(!far_away.off_screen);
}

BOOST_AUTO_TEST_CASE(Ortho_TheViewportSizeSetsTheAspect)
{
	scn::camera_component camera;
	camera.projection = scn::camera_projection::ortho;
	camera.ortho_size = 10.f;
	const auto s = make_scene({ 2.5f, 0, -1 }, {}, camera);
	BOOST_TEST(close_to(resolve(s, { 400, 400 }).position, { 300, 200 })); // half width 5: x = 0.75 of 400
	BOOST_TEST(close_to(resolve(s, { 800, 400 }).position, { 800 * (0.5f + 2.5f / 20.f), 200 }));
}

BOOST_AUTO_TEST_CASE(Ortho_BehindTheNearPlaneIsBehindTheCameraAndOutsideTheViewIsOffScreen)
{
	scn::camera_component camera;
	camera.projection = scn::camera_projection::ortho;
	camera.ortho_size = 10.f;
	const auto s = make_scene({ 0, 0, 1 }, {}, camera);
	auto r = resolve(s);
	BOOST_TEST(r.behind_camera);
	BOOST_TEST(r.off_screen);

	(*s.world).get<scn::world_transform>(s.target).world = glm::translate(glm::mat4{ 1.f }, glm::vec3{ 100, 0, -1 });
	r = resolve(s);
	BOOST_TEST(r.off_screen);
	BOOST_TEST(!r.behind_camera);
}

BOOST_AUTO_TEST_CASE(Projection_TheRendererAndTheMarkersShareTheMatrix)
{
	scn::camera_component persp;
	scn::camera_component ortho;
	ortho.projection = scn::camera_projection::ortho;
	ortho.ortho_size = 6.f;
	const glm::ivec4 viewport{ 0, 0, 640, 480 };
	BOOST_TEST((scn::camera_projection_matrix(persp, viewport) == glm::perspective(glm::radians(persp.fov), 640.f / 480.f, persp.near_distance, persp.far_distance)));
	const float half_w = 3.f * 640.f / 480.f;
	BOOST_TEST((scn::camera_projection_matrix(ortho, viewport) == glm::ortho(-half_w, half_w, -3.f, 3.f, ortho.near_distance, ortho.far_distance)));
}

// ---------------------------------------------------------------- the choice of the camera

BOOST_AUTO_TEST_CASE(Camera_TheLowestLayerWinsWhateverTheStorageOrder)
{
	auto& world = add_context("level", "world").state();
	auto& ui = add_context("level", "ui").state();
	scn::camera_component overlay;
	overlay.layer = 1;
	add_camera(world, { 100, 0, 0 }, overlay); // stored first, but on the higher layer
	add_camera(world, {}, scn::camera_component{});
	add_node(world, { 7 }, { 0, 0, -5 });
	const auto marker = add_marker(ui, "world", { 7 });

	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(close_to(result_for(ui, marker).position, { 400, 300 }));
}

BOOST_AUTO_TEST_CASE(Camera_ACameraThatDrawsNowhereIsNotUsed)
{
	auto& world = add_context("level", "world").state();
	auto& ui = add_context("level", "ui").state();
	// a game-view camera: redirected to a texture and switched off (zero viewport)
	scn::camera_component off;
	off.color_targets.push_back(res::tag(res::tag::memory, "__anchor_test_rt"));
	const auto camera = add_camera(world, {}, off);
	add_node(world, { 7 }, { 0, 0, -5 });
	const auto marker = add_marker(ui, "world", { 7 });

	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(!result_for(ui, marker).resolved);

	// the Game view gives it a viewport: the marker follows
	world.get<scn::camera_component>(camera).m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 200, 100 } };
	scn::resolve_screen_anchors(mgr, screen);
	const auto r = result_for(ui, marker);
	BOOST_TEST(r.resolved);
	BOOST_TEST(close_to(r.position, { 100, 50 }));
}

// ---------------------------------------------------------------- what resolves and what does not

BOOST_AUTO_TEST_CASE(Resolve_AnEmptyContextNamesTheContextOfTheMarker)
{
	auto& world = add_context("level", "world").state();
	add_camera(world);
	add_node(world, { 7 }, { 0, 0, -5 });
	const auto marker = add_marker(world, "", { 7 });
	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(close_to(result_for(world, marker).position, { 400, 300 }));
}

BOOST_AUTO_TEST_CASE(Resolve_ADanglingTargetIsUnresolvedAndOffScreen)
{
	auto& world = add_context("level", "world").state();
	auto& ui = add_context("level", "ui").state();
	add_camera(world);
	add_node(world, { 7 }, { 0, 0, -5 });
	const auto no_context = add_marker(ui, "nowhere", { 7 });
	const auto no_node = add_marker(ui, "world", { 8 });
	const auto no_path = add_marker(ui, "world", {});
	const auto child_path = add_marker(ui, "world", { 7, 1 }); // a path is the whole path, not a prefix
	scn::resolve_screen_anchors(mgr, screen);
	for (const auto marker : { no_context, no_node, no_path, child_path }) {
		const auto r = result_for(ui, marker);
		BOOST_TEST(!r.resolved);
		BOOST_TEST(r.off_screen);
	}
}

BOOST_AUTO_TEST_CASE(Resolve_ATargetWithoutATransformOrAWorldWithoutACameraIsUnresolved)
{
	auto& world = add_context("level", "world").state();
	auto& ui = add_context("level", "ui").state();
	scn::node_index::get(world);
	const auto bare = world.create();
	world.emplace<scn::node_id_component>(bare, scn::node_id_component{ { 7 } });
	add_camera(world);
	const auto no_transform = add_marker(ui, "world", { 7 });

	auto& empty = add_context("level", "empty").state();
	add_node(empty, { 7 }, { 0, 0, -5 });
	const auto no_camera = add_marker(ui, "empty", { 7 });

	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(!result_for(ui, no_transform).resolved);
	BOOST_TEST(!result_for(ui, no_camera).resolved);
}

BOOST_AUTO_TEST_CASE(Resolve_OnlyContextsThatAreExtractedHoldMarkers)
{
	const auto s = make_scene({ 0, 0, -5 });
	mgr.find("level", "ui")->set_extract(false);
	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(!s.ui->all_of<scn::screen_anchor_result>());

	mgr.find("level", "ui")->set_extract(true);
	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(s.ui->all_of<scn::screen_anchor_result>());
}

BOOST_AUTO_TEST_CASE(Resolve_AReferenceNeverReachesAnotherSet)
{
	mgr.create_set("play");
	// "world" exists in both sets with the target in different places; "only_play" only in "play"
	auto& level_world = add_context("level", "world").state();
	auto& level_ui = add_context("level", "ui").state();
	auto& play_world = add_context("play", "world").state();
	auto& play_ui = add_context("play", "ui").state();
	auto& only_play = add_context("play", "only_play").state();
	for (auto* reg : { &level_world, &play_world, &only_play }) add_camera(*reg);
	add_node(level_world, { 7 }, { 0, 0, -5 });
	add_node(play_world, { 7 }, { 2.5f, 0, -5 });
	add_node(only_play, { 7 }, { 0, 0, -5 });

	const auto level_marker = add_marker(level_ui, "world", { 7 });
	const auto play_marker = add_marker(play_ui, "world", { 7 });
	const auto stray = add_marker(level_ui, "only_play", { 7 });
	const auto mirrored = add_marker(play_ui, "ui", { 7 }); // names a context that has no such node

	scn::resolve_screen_anchors(mgr, screen);
	BOOST_TEST(close_to(result_for(level_ui, level_marker).position, { 400, 300 }));
	BOOST_TEST(close_to(result_for(play_ui, play_marker).position, { 550, 300 }));
	BOOST_TEST(!result_for(level_ui, stray).resolved);
	BOOST_TEST(!result_for(play_ui, mirrored).resolved);
}

// ---------------------------------------------------------------- no lag

BOOST_AUTO_TEST_CASE(NoLag_TheMarkerShowsTheTargetOfTheSameFrame)
{
	auto& world_context = add_context("level", "world", { "move_target" });
	auto& world = world_context.state();
	auto& ui = add_context("level", "ui").state();
	add_camera(world);
	const auto target = add_node(world, { 7 }, { 0, 0, -5 });
	world.ctx().emplace<mover>(mover{ target, 0.5f });
	const auto marker = add_marker(ui, "world", { 7 });

	for (int frame = 1; frame <= 5; ++frame) {
		mgr.update(one_frame); // the system moves the target by half a unit in this frame's update
		scn::resolve_screen_anchors(mgr, screen); // what the extract stage does next
		const float x = 0.5f * static_cast<float>(frame);
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST(close_to(world.get<scn::world_transform>(target).world[3].x, x, 0.0001f));
			// ndc.x = x / 5 at 4:3 and fov 90 is x / (5 * 4/3); the position is of this frame, not of the previous one
			const float expected = (0.5f + 0.5f * x / (5.f * 800.f / 600.f)) * 800.f;
			BOOST_TEST(close_to(result_for(ui, marker).position.x, expected));
		}
	}
}

BOOST_AUTO_TEST_CASE(NoLag_AMovingCameraAndAMovingTargetAreBothOfTheCurrentFrame)
{
	const auto s = make_scene({ 0, 0, -5 });
	for (int frame = 1; frame <= 3; ++frame) {
		const float x = static_cast<float>(frame);
		(*s.world).get<scn::world_transform>(s.camera).world = glm::translate(glm::mat4{ 1.f }, glm::vec3{ x, 0, 0 });
		(*s.world).get<scn::world_transform>(s.target).world = glm::translate(glm::mat4{ 1.f }, glm::vec3{ x, 0, -5 });
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST(close_to(resolve(s).position, { 400, 300 }));
		}
	}
}

// ---------------------------------------------------------------- the desc

BOOST_AUTO_TEST_CASE(Desc_TheMarkerIsAUserAddableComponent)
{
	BOOST_TEST(descs->is_registered("screen_anchor_desc"));
	BOOST_TEST(assembler->has_spawner("screen_anchor_desc"));
	const auto& names = descs->get_component_type_names();
	BOOST_TEST(std::count(names.begin(), names.end(), "screen_anchor_desc") == 1);
}

BOOST_AUTO_TEST_CASE(Desc_FieldsRoundTripAndBecomeTheComponent)
{
	auto created = descs->create_instance("screen_anchor_desc", res::tag{});
	BOOST_REQUIRE(created);
	created->deserialize(*descs, boost::json::parse(R"({"target_context":"world","target_path":[3,5],"offset":[0,2,0]})").as_object());
	auto& d = static_cast<scn::screen_anchor_desc&>(*created);
	BOOST_TEST(d.target_context == "world");
	BOOST_TEST((d.target_path == std::vector<std::uint64_t>{ 3, 5 }));
	BOOST_TEST((d.offset == glm::vec3{ 0, 2, 0 }));

	boost::json::object first;
	d.serialize(first);
	scn::screen_anchor_desc again;
	again.deserialize(*descs, first);
	boost::json::object second;
	again.serialize(second);
	BOOST_TEST(boost::json::value(first) == boost::json::value(second));

	entt::registry reg;
	const auto ent = reg.create();
	scn::assemble_screen_anchor(reg, ent, d, "marker");
	const auto& anchor = reg.get<scn::screen_anchor_component>(ent);
	BOOST_TEST((anchor.target == ecs::entity_ref{ "world", { 3, 5 } }));
	BOOST_TEST((anchor.offset == glm::vec3{ 0, 2, 0 }));
}

BOOST_AUTO_TEST_SUITE_END()
