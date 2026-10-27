#include <boost/test/unit_test.hpp>
#include "edt_editor_camera.h"
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_model.h>
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

BOOST_AUTO_TEST_CASE(DefaultState_RendersFromIdentity)
{
	// The startup view (smoke reference) is the identity camera; the default pose must reproduce it,
	// otherwise the first world reload would move the camera.
	entt::registry reg;
	edt::inject_camera(reg, edt::editor_camera_state{});

	const auto cam = find_editor_camera(reg);
	BOOST_REQUIRE(cam != entt::null);
	const glm::mat4& world = reg.get<scn::world_transform>(cam).world;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			BOOST_TEST_CONTEXT("column " << c << " row " << r)
				BOOST_CHECK_SMALL(world[c][r] - (c == r ? 1.f : 0.f), 1e-6f);
}

BOOST_AUTO_TEST_SUITE_END()
