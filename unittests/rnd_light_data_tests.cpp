#include <boost/test/unit_test.hpp>
#include "rnd_light_data.hpp"

BOOST_AUTO_TEST_SUITE(RndLightDataTests)

BOOST_AUTO_TEST_CASE(ToGpuParams_EmptyLights) {
	rnd::scene_lights_t lights;
	auto params = lights.to_gpu_params();
	// All directional_lights should be zero-initialized
	BOOST_TEST(params.directional_lights[0].direction.x == 0.0f);
	BOOST_TEST(params.directional_lights[0].diffuse.x == 0.0f);
}

BOOST_AUTO_TEST_CASE(ToGpuParams_SingleLight) {
	rnd::scene_lights_t lights;
	lights.directional.push_back({
		glm::vec4(0.0f, -1.0f, 0.0f, 0.0f),
		glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
		glm::vec4(0.2f, 0.2f, 0.2f, 1.0f),
		glm::vec4(0.5f, 0.5f, 0.5f, 1.0f)
	});
	auto params = lights.to_gpu_params();
	BOOST_TEST(params.directional_lights[0].direction.y == -1.0f);
	BOOST_TEST(params.directional_lights[0].diffuse.x == 1.0f);
	BOOST_TEST(params.directional_lights[0].ambient.x == 0.2f);
	BOOST_TEST(params.directional_lights[0].specular.x == 0.5f);
}

BOOST_AUTO_TEST_CASE(ToGpuParams_MultipleLights) {
	rnd::scene_lights_t lights;
	for (int i = 0; i < 3; ++i) {
		lights.directional.push_back({
			glm::vec4(float(i), 0.0f, 0.0f, 0.0f),
			glm::vec4(1.0f),
			glm::vec4(0.1f),
			glm::vec4(0.5f)
		});
	}
	auto params = lights.to_gpu_params();
	BOOST_TEST(params.directional_lights[0].direction.x == 0.0f);
	BOOST_TEST(params.directional_lights[1].direction.x == 1.0f);
	BOOST_TEST(params.directional_lights[2].direction.x == 2.0f);
}

BOOST_AUTO_TEST_CASE(ToGpuParams_ClampsToMaxLightCount) {
	rnd::scene_lights_t lights;
	for (int i = 0; i < rnd::lights_params::MAX_LIGHT_COUNT + 5; ++i) {
		lights.directional.push_back({
			glm::vec4(float(i), 0.0f, 0.0f, 0.0f),
			glm::vec4(1.0f),
			glm::vec4(0.1f),
			glm::vec4(0.5f)
		});
	}
	auto params = lights.to_gpu_params();
	// Last valid light should be at MAX_LIGHT_COUNT - 1
	BOOST_TEST(params.directional_lights[rnd::lights_params::MAX_LIGHT_COUNT - 1].direction.x ==
		float(rnd::lights_params::MAX_LIGHT_COUNT - 1));
}

BOOST_AUTO_TEST_SUITE_END()
