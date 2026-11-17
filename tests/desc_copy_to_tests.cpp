#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <memory>
#include <string_view>
#include <boost/json.hpp>

namespace json = boost::json;

namespace {

	json::object serialized(const desc::desc_base& value)
	{
		json::object out;
		value.serialize(out);
		return out;
	}

}

BOOST_AUTO_TEST_SUITE(DescCopyToTests)

BOOST_FIXTURE_TEST_CASE(CopyToAcrossMismatchedHandwrittenTypesDoesNotCorrupt, test_util::scene_env)
{
	constexpr std::string_view types[] = {
		"directional_light_desc", "camera_desc", "anchor_desc", "object_desc", "bone_desc",
		"animation_controller_desc", "animated_node_desc", "skinning_desc",
	};
	for (const auto from : types) {
		for (const auto to : types) {
			auto source = descs->create_instance(from, res::tag{});
			auto target = descs->create_instance(to, res::tag{});
			BOOST_REQUIRE_MESSAGE(source && target, from << " -> " << to);
			BOOST_CHECK_NO_THROW(source->copy_to(*target));
			BOOST_CHECK_EQUAL(target->get_type(), to);
		}
	}
}

BOOST_FIXTURE_TEST_CASE(CopyToAcrossMismatchedTypesLeavesUnrelatedFieldsAlone, test_util::scene_env)
{
	auto controller = std::dynamic_pointer_cast<scn::animation_controller_desc>(descs->create_instance("animation_controller_desc", res::tag{}));
	auto light = std::dynamic_pointer_cast<scn::directional_light_desc>(descs->create_instance("directional_light_desc", res::tag{}));
	auto skinning = std::dynamic_pointer_cast<scn::skinning_desc>(descs->create_instance("skinning_desc", res::tag{}));
	auto bone = std::dynamic_pointer_cast<scn::bone_desc>(descs->create_instance("bone_desc", res::tag{}));
	BOOST_REQUIRE(controller && light && skinning && bone);

	controller->speed = 3.0f;
	const glm::vec4 before = light->diffuse;
	controller->copy_to(*light);
	BOOST_TEST((light->diffuse == before));

	skinning->bone_count = 12;
	bone->index = 4;
	skinning->copy_to(*bone);
	BOOST_TEST(bone->index == 4);
}

BOOST_AUTO_TEST_CASE(CopyToWithoutADescSystemSkipsInsteadOfTouchingTheWrongType)
{
	scn::directional_light_desc light;
	scn::camera_desc camera;
	const json::object before = serialized(camera);
	light.copy_to(camera);
	BOOST_TEST(json::value(serialized(camera)) == json::value(before));
}

BOOST_AUTO_TEST_CASE(CopyToSameTypeStillCopiesMemberWise)
{
	scn::directional_light_desc source;
	source.diffuse = { 0.1f, 0.2f, 0.3f, 1.0f };
	scn::directional_light_desc target;
	source.copy_to(target);
	BOOST_TEST((target.diffuse == source.diffuse));
}

BOOST_AUTO_TEST_SUITE_END()
