#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string_view>

namespace json = boost::json;

// Bug #545 (F19, F20): the transform of a camera node is the node's own; camera_desc must not replace it.

namespace {

struct camera_transform_fixture : test_util::scene_env
{
	static entt::entity find_named(entt::registry& reg, std::string_view name)
	{
		for (auto [e, n] : reg.view<scn::name_component>().each()) {
			if (n.name == name) return e;
		}
		return entt::null;
	}

	static glm::vec3 position_of(entt::registry& reg, entt::entity e)
	{
		return glm::vec3(reg.get<scn::local_transform>(e).local[3]);
	}

	json::object migrated(const char* text)
	{
		auto obj = json::parse(text).as_object();
		descs->migrate_tree(obj, "test");
		return obj;
	}

	static double number(const json::value& array, std::size_t index) { return array.as_array().at(index).to_number<double>(); }
};

}

BOOST_FIXTURE_TEST_SUITE(ScnCameraTransformTests, camera_transform_fixture)

BOOST_AUTO_TEST_CASE(ACameraNodeKeepsItsOwnTransformWhenSpawned)
{
	const auto tag = put("cam.prefab.desc", R"({"__type":"prefab_desc","children":[
		{"__id":"00000000000000c1","name":"Cam","transform":{"position":[0,5,0]},
		 "components":{"camera":{"__type":"camera_desc"}}}]})");
	entt::registry reg;
	const auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *load<scn::prefab_desc>(tag), "inst", tag);

	const auto cam = find_named(reg, "Cam");
	BOOST_REQUIRE(cam != entt::null);
	BOOST_REQUIRE(reg.all_of<scn::camera_component>(cam));
	BOOST_TEST((position_of(reg, cam) == glm::vec3(0.f, 5.f, 0.f)));
}

BOOST_AUTO_TEST_CASE(AssembleCameraDoesNotTouchAnExistingLocalTransform)
{
	entt::registry reg;
	const auto e = reg.create();
	const glm::mat4 placed = glm::translate(glm::mat4(1.0f), glm::vec3(1.f, 2.f, 3.f));
	reg.emplace<scn::local_transform>(e, placed);
	scn::assemble_camera(reg, e, scn::camera_desc{}, "cam");
	BOOST_TEST((reg.get<scn::local_transform>(e).local == placed));
}

// ---- migration camera_desc v1 -> v2: the pose that was rendered moves into the node (LEGACY adapter, see #28)

BOOST_AUTO_TEST_CASE(TheMigrationWritesTheRenderedPoseIntoTheNodeAndDropsTheCameraTransform)
{
	// v1 put the camera 2 units behind camera_desc.transform.position along its own Z: eye = (0,1,3) + Rx(-5 deg)(0,0,2).
	auto world = migrated(R"({"__type":"world_desc","__version":3,"name":"w","children":[
		{"__id":"00000000000000c1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":1,
			"fov":50,"transform":{"position":[0,1,3],"rotation":[-5,0,0]}}}}]})");
	BOOST_TEST(world.at("__version").as_int64() == 4);

	const auto& node = world.at("children").as_array()[0].as_object();
	const auto& position = node.at("transform").at("position");
	BOOST_CHECK_SMALL(number(position, 0), 1e-4);
	BOOST_TEST(number(position, 1) == 1.174311, boost::test_tools::tolerance(1e-4));
	BOOST_TEST(number(position, 2) == 4.992389, boost::test_tools::tolerance(1e-4));
	const auto& rotation = node.at("transform").at("rotation");
	BOOST_TEST(number(rotation, 0) == -5.0, boost::test_tools::tolerance(1e-4));
	BOOST_CHECK_SMALL(number(rotation, 1), 1e-3);
	BOOST_CHECK_SMALL(number(rotation, 2), 1e-3);

	const auto& camera = node.at("components").at("camera").as_object();
	BOOST_TEST(!camera.contains("transform"));
	BOOST_TEST(camera.at("__version").as_int64() == 2);
	BOOST_TEST(camera.at("fov").to_number<double>() == 50.0);
}

BOOST_AUTO_TEST_CASE(ACameraWithoutATransformRenderedTwoUnitsBehindTheOrigin)
{
	auto world = migrated(R"({"__type":"world_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"Cam","transform":{"position":[9,9,9],"scale":[2,2,2]},
		 "components":{"camera":{"__type":"camera_desc"}}}]})");
	const auto& transform = world.at("children").as_array()[0].at("transform").as_object();
	BOOST_CHECK_SMALL(number(transform.at("position"), 0), 1e-6);
	BOOST_CHECK_SMALL(number(transform.at("position"), 1), 1e-6);
	BOOST_TEST(number(transform.at("position"), 2) == 2.0, boost::test_tools::tolerance(1e-6));
	BOOST_TEST(!transform.contains("scale")); // the node's own transform was never used by the camera
}

BOOST_AUTO_TEST_CASE(TheMigrationLeavesAMigratedCameraAndOtherComponentsAlone_AndIsIdempotent)
{
	const char* text = R"({"__type":"world_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"Cam","transform":{"position":[5,5,5]},
		 "components":{"camera":{"__type":"camera_desc","__version":2,"fov":40}}},
		{"__id":"00000000000000c2","name":"Lamp","transform":{"position":[1,2,3]},
		 "components":{"light":{"__type":"directional_light_desc"}}}]})";
	const auto world = migrated(text);
	const auto& children = world.at("children").as_array();
	BOOST_TEST(number(children[0].at("transform").at("position"), 0) == 5.0);
	BOOST_TEST(number(children[1].at("transform").at("position"), 2) == 3.0);

	auto again = world;
	descs->migrate_tree(again, "test");
	BOOST_TEST(json::value(again) == json::value(world));
}

BOOST_AUTO_TEST_CASE(ACameraDescOutsideANodeLosesItsTransformWithAWarning)
{
	const auto camera = migrated(R"({"__type":"camera_desc","fov":50,"transform":{"position":[1,2,3],"rotation":[0,90,0]}})");
	BOOST_TEST(!camera.contains("transform"));
	BOOST_TEST(camera.at("__version").as_int64() == 2);
	BOOST_TEST(camera.at("fov").to_number<double>() == 50.0);
}

// The point of the migration: a level made before renders from the same place as it did.
BOOST_AUTO_TEST_CASE(AMigratedLegacyCameraSitsWhereTheOldAssemblyPutIt)
{
	const glm::vec3 pivot{ 1.f, 2.f, 3.f };
	const glm::vec3 degrees{ 30.f, 45.f, 10.f };
	const auto tag = put("legacy.prefab.desc", R"({"__type":"prefab_desc","__version":3,"children":[
		{"__id":"00000000000000c1","name":"Cam","transform":{"position":[9,9,9],"scale":[2,2,2]},
		 "components":{"camera":{"__type":"camera_desc","__version":1,
			"transform":{"position":[1,2,3],"rotation":[30,45,10]}}}}]})");
	entt::registry reg;
	const auto root = reg.create();
	scn::assemble_prefab(*assembler, reg, root, *load<scn::prefab_desc>(tag), "inst", tag);
	const auto cam = find_named(reg, "Cam");
	BOOST_REQUIRE(cam != entt::null);

	const glm::mat4 legacy = glm::translate(glm::mat4(1.f), pivot) * glm::mat4_cast(glm::quat(glm::radians(degrees)))
		* glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 2.f));
	const glm::mat4& actual = reg.get<scn::local_transform>(cam).local;
	for (int column = 0; column < 4; ++column) {
		for (int row = 0; row < 4; ++row) {
			BOOST_TEST_CONTEXT("column " << column << " row " << row)
				BOOST_CHECK_SMALL(actual[column][row] - legacy[column][row], 1e-3f);
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
