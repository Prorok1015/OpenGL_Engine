#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/scn_geometry_holds.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_model.h>
#include <entt/entt.hpp>

// #1021: a mesh entity holds the geometry it draws as long as its geometry_component names it.

namespace {

struct holds_env : test_util::scene_env
{
	holds_env() { descs->register_desc<rnd::geometry_desc>("geometry_desc"); }
	~holds_env() { descs->unregister_desc("geometry_desc"); }

	scn::mesh_node_desc mesh_of(std::string_view name)
	{
		scn::mesh_node_desc desc;
		desc.geometry = require_loaded<rnd::geometry_desc>(
			put(name, R"({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[0,1,2],"vertices":[]})"));
		BOOST_REQUIRE(desc.geometry.is_ready());
		return desc;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ScnGeometryHolds, holds_env)

BOOST_AUTO_TEST_CASE(AnAssembledMesh_HoldsItsGeometry_AndAnotherAssemblyTakesTheNewOne)
{
	entt::registry reg;
	const auto e = reg.create();
	const scn::mesh_node_desc first = mesh_of("a.geom.desc");
	scn::assemble_mesh_node(reg, e, first, "mesh");
	const auto* held = scn::geometry_holds::of(reg).find(e);
	BOOST_REQUIRE(held);
	BOOST_TEST(held->get_tag() == first.geometry.get_tag());

	const scn::mesh_node_desc second = mesh_of("b.geom.desc");
	scn::assemble_mesh_node(reg, e, second, "mesh");
	held = scn::geometry_holds::of(reg).find(e);
	BOOST_REQUIRE(held);
	BOOST_TEST(held->get_tag() == second.geometry.get_tag());
	BOOST_TEST(scn::geometry_holds::of(reg).size() == 1u);
}

BOOST_AUTO_TEST_CASE(AnotherTagInTheComponent_LetsGoOfTheGeometry)
{
	entt::registry reg;
	const auto e = reg.create();
	scn::assemble_mesh_node(reg, e, mesh_of("a.geom.desc"), "mesh");
	BOOST_REQUIRE(scn::geometry_holds::of(reg).find(e));

	reg.replace<scn::geometry_component>(e, scn::geometry_component{ .geom_tag = tag_of("other.geom.desc") });
	BOOST_TEST(!scn::geometry_holds::of(reg).find(e));
}

BOOST_AUTO_TEST_CASE(TheSameTagAgain_KeepsTheGeometry)
{
	entt::registry reg;
	const auto e = reg.create();
	const scn::mesh_node_desc desc = mesh_of("a.geom.desc");
	scn::assemble_mesh_node(reg, e, desc, "mesh");
	reg.patch<scn::geometry_component>(e, [](scn::geometry_component&) {});
	BOOST_TEST(scn::geometry_holds::of(reg).find(e));
}

BOOST_AUTO_TEST_CASE(ARemovedComponentOrEntity_LetsGoOfTheGeometry)
{
	entt::registry reg;
	const auto removed = reg.create();
	const auto destroyed = reg.create();
	scn::assemble_mesh_node(reg, removed, mesh_of("a.geom.desc"), "mesh");
	scn::assemble_mesh_node(reg, destroyed, mesh_of("b.geom.desc"), "mesh");
	BOOST_REQUIRE(scn::geometry_holds::of(reg).size() == 2u);

	reg.remove<scn::geometry_component>(removed);
	reg.destroy(destroyed);
	BOOST_TEST(scn::geometry_holds::of(reg).size() == 0u);
}

// A component made by any other path (a snapshot restored, a script) is held through the resources the registry was
// attached with, as long as the renderer would read its tag.
BOOST_AUTO_TEST_CASE(AnAttachedRegistry_HoldsAComponentMadeWithoutAnAssembly)
{
	entt::registry reg;
	scn::geometry_holds::attach(reg, rs.get());
	const scn::mesh_node_desc stored = mesh_of("a.geom.desc");
	const auto made = reg.create();
	reg.emplace<scn::geometry_component>(made, scn::geometry_component{ .geom_tag = stored.geometry.get_tag() });
	const auto* held = scn::geometry_holds::of(reg).find(made);
	BOOST_REQUIRE(held);
	BOOST_TEST(held->get_tag() == stored.geometry.get_tag());

	const scn::mesh_node_desc other = mesh_of("b.geom.desc");
	reg.replace<scn::geometry_component>(made, scn::geometry_component{ .geom_tag = other.geometry.get_tag() });
	held = scn::geometry_holds::of(reg).find(made);
	BOOST_REQUIRE(held);
	BOOST_TEST(held->get_tag() == other.geometry.get_tag());

	const auto refused = reg.create();
	const res::tag user_file{ "user://saves/slot0.desc" };
	reg.emplace<scn::geometry_component>(refused, scn::geometry_component{ .geom_tag = user_file });
	BOOST_TEST(!scn::geometry_holds::of(reg).find(refused));
}

BOOST_AUTO_TEST_CASE(AMeshWithoutGeometry_HoldsNothing)
{
	entt::registry reg;
	const auto e = reg.create();
	scn::mesh_node_desc desc;
	scn::assemble_mesh_node(reg, e, desc, "mesh");
	BOOST_TEST(!reg.ctx().contains<scn::geometry_holds>());
}

BOOST_AUTO_TEST_SUITE_END()
