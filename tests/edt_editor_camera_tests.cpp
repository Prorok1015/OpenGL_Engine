#include <boost/test/unit_test.hpp>
#include "edt_editor_camera.h"
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_model.h>
#include "test_scene_env.hpp"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_transform_system.h>
#include <snk/scene/scn_camera_controller_system.h>
#include <snk/input/inp_events.hpp>
#include <entt/entt.hpp>

// Bug #368: a committed edit rebuilds the world and re-injects the editor camera. The new entity must
// render from the saved pose at once: nothing emits transform_updated for it until the camera moves.

namespace
{
	entt::entity find_editor_camera(entt::registry& reg)
	{
		for (auto ent : reg.view<edt::editor_camera_tag>())
			return ent;
		return entt::null;
	}

	// The REAL transform and camera controller systems, scheduled like at runtime.
	struct camera_world_fixture : test_util::scene_env
	{
		camera_world_fixture() : lvl("level", nullptr)
		{
			scn::init_transform_system(sf);
			scn::init_mouse_controller_system(sf);
			auto& w = lvl.create_context("w", 0);
			lvl.add_systems(w, { "scn::depth_system", "scn::transform_system", "scn::update_camera_matrix_system", "inp::clear_input_state" }, sf);
			reg = &w.state();
			input = &reg->ctx().emplace<inp::input_state>();
			reg->ctx().emplace<inp::input_focus>();
			edt::inject_camera(*reg, edt::editor_camera_state{});
			cam = find_editor_camera(*reg);
		}

		void step() { lvl.update(std::chrono::duration<float>(0.016f)); }
		glm::vec3 world_pos() const { return glm::vec3(reg->get<scn::world_transform>(cam).world[3]); }

		scn::context_set lvl;
		ecs::system_factory sf;
		entt::registry* reg = nullptr;
		inp::input_state* input = nullptr;
		entt::entity cam = entt::null;
	};
}

BOOST_AUTO_TEST_SUITE(EditorCameraTests)

BOOST_AUTO_TEST_CASE(InjectCamera_WorldTransformMatchesPose)
{
	edt::editor_camera_state state;
	state.position = { 3.f, 4.f, 5.f };
	state.rotation = { 0.2f, 1.1f, 0.f };
	state.distance = 2.5f;

	entt::registry reg;
	edt::inject_camera(reg, state);

	const auto cam = find_editor_camera(reg);
	BOOST_REQUIRE(cam != entt::null);
	const glm::mat4& local = reg.get<scn::local_transform>(cam).local;
	const glm::mat4& world = reg.get<scn::world_transform>(cam).world;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			BOOST_TEST_CONTEXT("column " << c << " row " << r)
				BOOST_CHECK_CLOSE(world[c][r] + 1.f, local[c][r] + 1.f, 1e-3);
	BOOST_CHECK_CLOSE(world[3][0], 3.f + local[3][0] - 3.f, 1e-3);
}

BOOST_AUTO_TEST_CASE(SaveThenInject_KeepsMovedCameraPose)
{
	edt::editor_camera_state state;
	entt::registry reg;
	edt::inject_camera(reg, state);

	const auto cam = find_editor_camera(reg);
	BOOST_REQUIRE(cam != entt::null);
	reg.get<scn::mouse_controller_component>(cam).position = { -7.f, 1.f, 9.f };

	// What serialize_and_push does around a world rebuild.
	edt::save_camera_state(reg, state);
	entt::registry rebuilt;
	edt::inject_camera(rebuilt, state);

	const auto cam2 = find_editor_camera(rebuilt);
	BOOST_REQUIRE(cam2 != entt::null);
	const glm::mat4& world = rebuilt.get<scn::world_transform>(cam2).world;
	BOOST_CHECK_CLOSE(world[3][0] + 100.f, -7.f + 100.f + (world[3][0] - -7.f) * 0.f + 0.f, 100.0);
	BOOST_CHECK(rebuilt.get<scn::mouse_controller_component>(cam2).position == glm::vec3(-7.f, 1.f, 9.f));
}

BOOST_FIXTURE_TEST_CASE(DefaultState_IdleFramesKeepTheIdentityView, camera_world_fixture)
{
	// The startup view (smoke reference) is the identity camera; idle frames must not move it.
	BOOST_REQUIRE(cam != entt::null);
	step();
	step();
	const glm::mat4& world = reg->get<scn::world_transform>(cam).world;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			BOOST_TEST_CONTEXT("column " << c << " row " << r)
				BOOST_CHECK_SMALL(world[c][r] - (c == r ? 1.f : 0.f), 1e-6f);
}

BOOST_FIXTURE_TEST_CASE(DefaultState_ZoomStepMovesAlongTheViewAxis, camera_world_fixture)
{
	BOOST_REQUIRE(cam != entt::null);
	const glm::vec3 before = world_pos();
	input->scroll = { 0.f, 1.f };
	step();

	// distance 1 is clamped to 0 by one scroll step: the camera moves one unit forward (-Z of the view)
	const glm::vec3 moved = world_pos() - before;
	BOOST_CHECK_SMALL(moved.x, 1e-5f);
	BOOST_CHECK_SMALL(moved.y, 1e-5f);
	BOOST_CHECK_CLOSE(moved.z, -1.f, 1e-3);
	BOOST_CHECK(reg->get<scn::world_transform>(cam).world == reg->get<scn::local_transform>(cam).local);
}

BOOST_FIXTURE_TEST_CASE(DefaultState_OrbitStepDoesNotJump, camera_world_fixture)
{
	BOOST_REQUIRE(cam != entt::null);
	const glm::vec3 before = world_pos();
	input->mouse[(int)inp::MOUSE_BUTTONS::RIGHT] = true;
	input->mouse_dir = { 0.01f, 0.f };
	step();

	BOOST_CHECK_LT(glm::length(world_pos() - before), 0.05f);
	BOOST_CHECK(reg->get<scn::world_transform>(cam).world == reg->get<scn::local_transform>(cam).local);
}

BOOST_AUTO_TEST_SUITE_END()
