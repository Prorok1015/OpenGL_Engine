#include <boost/test/unit_test.hpp>
#include "rnd_light_data.hpp"
#include <cstring>
#include <cstddef>

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

BOOST_AUTO_TEST_CASE(Std140_BytesAtOffsets) {
	rnd::scene_lights_t lights;
	for (int i = 0; i < 2; ++i) {
		lights.directional.push_back({
			glm::vec4(float(i), 0, 0, 0), glm::vec4(float(i) + 10, 0, 0, 0),
			glm::vec4(0), glm::vec4(0)
		});
	}
	auto params = lights.to_gpu_params();
	for (int i = 0; i < 3; ++i) {
		params.point_lights[i].position = glm::vec4(float(i) + 100, 0, 0, 0);
		params.point_lights[i].quadratic = float(i) + 200;
	}
	params.point_light_count = 3;

	auto read_f = [&](std::size_t off) {
		float v;
		std::memcpy(&v, reinterpret_cast<const std::byte*>(&params) + off, sizeof(v));
		return v;
	};
	auto read_i = [&](std::size_t off) {
		int v;
		std::memcpy(&v, reinterpret_cast<const std::byte*>(&params) + off, sizeof(v));
		return v;
	};

	// std140: DirectionalLight stride 64, PointLight stride 80, counts right after the arrays
	BOOST_TEST(read_f(1 * 64) == 1.0f);
	BOOST_TEST(read_f(1 * 64 + 16) == 11.0f);
	BOOST_TEST(read_f(768 + 2 * 80) == 102.0f);
	BOOST_TEST(read_f(768 + 2 * 80 + 72) == 202.0f);
	BOOST_TEST(read_i(1728) == 2);
	BOOST_TEST(read_i(1732) == 3);
}

BOOST_AUTO_TEST_SUITE_END()
