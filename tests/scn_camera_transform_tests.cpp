#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string_view>

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

BOOST_AUTO_TEST_SUITE_END()
