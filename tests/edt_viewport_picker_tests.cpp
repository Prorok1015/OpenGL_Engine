#include <boost/test/unit_test.hpp>
#include "edt_viewport_picker.h"
#include "test_quad_geometry.hpp"
#include "test_scene_env.hpp"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_model.h>
#include <glm/gtc/matrix_transform.hpp>
#include <entt/entt.hpp>
#include <cmath>

// #1189: the pixel of the editor camera's image -> the node under it, over the engine's raycast.

namespace {

struct picker_env : test_util::scene_env
{
	picker_env()
	{
		descs->register_desc<rnd::geometry_desc>("geometry_desc");
		geometry = require_loaded<rnd::geometry_desc>(put("quad.geom.desc", test_util::quad_geometry_json(1.f)));
		BOOST_REQUIRE(geometry.is_ready());
		scn::geometry_holds::attach(reg, rs.get());
	}
	~picker_env() { descs->unregister_desc("geometry_desc"); }

	// A unit-radius square at `position`, facing +Z.
	entt::entity mesh_at(glm::vec3 position, bool visible = true)
	{
		const auto entity = reg.create();
		reg.emplace<scn::world_transform>(entity, scn::world_transform{ glm::translate(glm::mat4{ 1.f }, position) });
		if (visible) {
			reg.emplace<scn::renderable>(entity);
		}
		reg.emplace<scn::mesh_component>(entity, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 6 } });
		reg.emplace<scn::geometry_component>(entity, scn::geometry_component{ .geom_tag = geometry.get_tag() });
		return entity;
	}

	entt::entity node_mesh_at(glm::vec3 position, std::uint64_t id)
	{
		const auto entity = mesh_at(position);
		reg.emplace<scn::node_id_component>(entity, scn::node_id_component{ { id } });
		return entity;
	}

	// The camera at (0, 0, 10) looking down -Z, a 100 x 50 image.
	edt::pick_point at_pixel(glm::vec2 pixel) const
	{
		edt::pick_point point;
		point.view = glm::lookAt(glm::vec3{ 0.f, 0.f, 10.f }, glm::vec3{ 0.f }, glm::vec3{ 0.f, 1.f, 0.f });
		point.projection = glm::perspective(glm::radians(60.f), 2.f, 0.1f, 100.f);
		point.pixel = pixel;
		point.image_size = { 100.f, 50.f };
		return point;
	}

	entt::registry reg;
	res::res_handle<rnd::geometry_desc> geometry;
};

}

BOOST_FIXTURE_TEST_SUITE(EdtViewportPicker, picker_env)

BOOST_AUTO_TEST_CASE(TheMiddleOfTheImage_PicksTheNodeInFrontOfTheCamera)
{
	const auto node = node_mesh_at({ 0.f, 0.f, 0.f }, 7);
	const auto picked = edt::pick_node(reg, at_pixel({ 50.f, 25.f }));
	BOOST_REQUIRE(picked);
	BOOST_TEST((picked->entity == node));
	BOOST_TEST(picked->path == std::vector<std::uint64_t>{ 7 });
}

BOOST_AUTO_TEST_CASE(ACornerOfTheImage_MissesAndPicksNothing)
{
	node_mesh_at({ 0.f, 0.f, 0.f }, 7);
	BOOST_TEST(!edt::pick_node(reg, at_pixel({ 2.f, 2.f })).has_value());
}

BOOST_AUTO_TEST_CASE(OfTwoOverlappingNodes_TheNearerToTheCameraWins)
{
	node_mesh_at({ 0.f, 0.f, -3.f }, 1);
	const auto near_node = node_mesh_at({ 0.f, 0.f, 3.f }, 2);
	const auto picked = edt::pick_node(reg, at_pixel({ 50.f, 25.f }));
	BOOST_REQUIRE(picked);
	BOOST_TEST((picked->entity == near_node));
	BOOST_TEST(picked->path == std::vector<std::uint64_t>{ 2 });
}

BOOST_AUTO_TEST_CASE(AHiddenMesh_IsNotPicked_TheOneBehindItIs)
{
	const auto behind = node_mesh_at({ 0.f, 0.f, -3.f }, 1);
	const auto hidden = node_mesh_at({ 0.f, 0.f, 3.f }, 2);
	reg.remove<scn::renderable>(hidden);
	const auto picked = edt::pick_node(reg, at_pixel({ 50.f, 25.f }));
	BOOST_REQUIRE(picked);
	BOOST_TEST((picked->entity == behind));
}

BOOST_AUTO_TEST_CASE(AMeshThatIsNoNode_BelongsToItsNearestAncestorThatIs)
{
	const auto root = reg.create();
	reg.emplace<scn::node_id_component>(root, scn::node_id_component{ { 4, 9 } });
	const auto part = mesh_at({ 0.f, 0.f, 0.f });
	reg.emplace<scn::parent_component>(part, scn::parent_component{ root });
	const auto picked = edt::pick_node(reg, at_pixel({ 50.f, 25.f }));
	BOOST_REQUIRE(picked);
	BOOST_TEST((picked->entity == root));
	BOOST_TEST(picked->path == (std::vector<std::uint64_t>{ 4, 9 }));
}

BOOST_AUTO_TEST_CASE(AnEditorsOwnMesh_WithNoNodeAbove_IsSeenThrough)
{
	const auto behind = node_mesh_at({ 0.f, 0.f, -3.f }, 1);
	mesh_at({ 0.f, 0.f, 3.f });
	const auto picked = edt::pick_node(reg, at_pixel({ 50.f, 25.f }));
	BOOST_REQUIRE(picked);
	BOOST_TEST((picked->entity == behind));
}

BOOST_AUTO_TEST_CASE(ARotatedAndScaledNode_IsPickedWhereItIs_AndMissedBesideIt)
{
	const auto node = reg.create();
	glm::mat4 matrix = glm::translate(glm::mat4{ 1.f }, glm::vec3{ 4.f, 0.f, 0.f });
	matrix = glm::rotate(matrix, glm::radians(30.f), glm::vec3{ 0.f, 0.f, 1.f });
	matrix = glm::scale(matrix, glm::vec3{ 0.5f, 2.f, 1.f });
	reg.emplace<scn::world_transform>(node, scn::world_transform{ matrix });
	reg.emplace<scn::renderable>(node);
	reg.emplace<scn::mesh_component>(node, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 6 } });
	reg.emplace<scn::geometry_component>(node, scn::geometry_component{ .geom_tag = geometry.get_tag() });
	reg.emplace<scn::node_id_component>(node, scn::node_id_component{ { 3 } });

	// The camera's image at z = 0 spans x in [-5.77 * 2, 5.77 * 2]: x = 4 is at 50 + 4 / (2 * 5.77) * 50 px.
	const float center_x = 50.f + 4.f / (2.f * 10.f * std::tan(glm::radians(30.f))) * 50.f;
	BOOST_TEST(edt::pick_node(reg, at_pixel({ center_x, 25.f })).has_value());
	BOOST_TEST(!edt::pick_node(reg, at_pixel({ center_x + 25.f, 25.f })).has_value());
}

BOOST_AUTO_TEST_CASE(AMeshWhoseGeometryIsNotLoaded_IsNotPicked)
{
	const auto entity = reg.create();
	reg.emplace<scn::world_transform>(entity);
	reg.emplace<scn::renderable>(entity);
	reg.emplace<scn::mesh_component>(entity, scn::mesh_component{ scn::mesh_view{ 0, 4, 0, 6 } });
	reg.emplace<scn::geometry_component>(entity, scn::geometry_component{ .geom_tag = tag_of("missing.geom.desc") });
	reg.emplace<scn::node_id_component>(entity, scn::node_id_component{ { 5 } });
	BOOST_TEST(!edt::pick_node(reg, at_pixel({ 50.f, 25.f })).has_value());
}

BOOST_AUTO_TEST_CASE(AnEmptyImageOrASingularMatrix_PicksNothing)
{
	node_mesh_at({ 0.f, 0.f, 0.f }, 7);
	edt::pick_point empty = at_pixel({ 50.f, 25.f });
	empty.image_size = { 0.f, 50.f };
	BOOST_TEST(!edt::pick_node(reg, empty).has_value());
	edt::pick_point singular = at_pixel({ 50.f, 25.f });
	singular.view = glm::mat4{ 0.f };
	BOOST_TEST(!edt::pick_node(reg, singular).has_value());
}

BOOST_AUTO_TEST_SUITE_END()
