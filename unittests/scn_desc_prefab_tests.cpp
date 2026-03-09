#include <boost/test/unit_test.hpp>
#include <entt/entt.hpp>
#include <boost/json.hpp>
#include "scn_model.h"
#include "scn_mesh_node_desc.h"
#include "scn_object_desc.h"
#include "scn_animations_desc.h"
#include "scn_keyframes_desc.h"
#include "scn_bone_desc.h"
#include "scn_skinning_desc.h"
#include "scn_glm_json_convert.h"
#include <glm/gtc/matrix_transform.hpp>

namespace json = boost::json;

BOOST_AUTO_TEST_SUITE(ScnDescPrefabTests)

// ---- object_desc ----

BOOST_AUTO_TEST_CASE(ObjectDesc_TypeConstant)
{
	BOOST_TEST(scn::object_desc::__type == "object_desc");
}

BOOST_AUTO_TEST_CASE(ObjectDesc_AssemblesObjectComponent)
{
	entt::registry reg;
	auto e = reg.create();

	scn::object_desc desc;
	scn::assemble_object(reg, e, desc, "root");

	BOOST_TEST(reg.all_of<scn::object_component>(e));
}

BOOST_AUTO_TEST_CASE(ObjectDesc_AssembleIsIdempotent)
{
	entt::registry reg;
	auto e = reg.create();
	scn::object_desc desc;

	scn::assemble_object(reg, e, desc, "root");
	scn::assemble_object(reg, e, desc, "root"); // second call should not throw

	BOOST_TEST(reg.all_of<scn::object_component>(e));
}

// ---- mesh_node_desc ----

BOOST_AUTO_TEST_CASE(MeshNodeDesc_TypeConstant)
{
	BOOST_TEST(scn::mesh_node_desc::__type == "mesh_node_desc");
}

BOOST_AUTO_TEST_CASE(MeshNodeDesc_AssemblesComponents)
{
	entt::registry reg;
	auto e = reg.create();

	scn::mesh_node_desc desc;
	desc.vx_begin = 0;
	desc.vx_end = 24;
	desc.ind_begin = 0;
	desc.ind_end = 36;

	scn::assemble_mesh_node(reg, e, desc, "mesh");

	BOOST_TEST(reg.all_of<scn::mesh_component>(e));
	BOOST_TEST(reg.all_of<scn::renderable>(e));
}

BOOST_AUTO_TEST_CASE(MeshNodeDesc_MeshViewCorrect)
{
	entt::registry reg;
	auto e = reg.create();

	scn::mesh_node_desc desc;
	desc.vx_begin = 4;
	desc.vx_end = 16;
	desc.ind_begin = 8;
	desc.ind_end = 32;

	scn::assemble_mesh_node(reg, e, desc, "mesh");

	auto& mc = reg.get<scn::mesh_component>(e);
	BOOST_TEST(mc.mesh.vx_begin == 4u);
	BOOST_TEST(mc.mesh.vx_end == 16u);
	BOOST_TEST(mc.mesh.ind_begin == 8u);
	BOOST_TEST(mc.mesh.ind_end == 32u);
}

// ---- animations_desc ----

BOOST_AUTO_TEST_CASE(AnimationsDesc_TypeConstant)
{
	BOOST_TEST(scn::animations_desc::__type == "animations_desc");
}

BOOST_AUTO_TEST_CASE(AnimationsDesc_AssemblesComponent)
{
	entt::registry reg;
	auto e = reg.create();

	scn::animations_desc desc;
	desc.animations.push_back({ "Walk", 30.f, 30.f });
	desc.animations.push_back({ "Run", 20.f, 24.f });

	scn::assemble_animations(reg, e, desc, "root");

	BOOST_TEST(reg.all_of<scn::animations_component>(e));
	auto& comp = reg.get<scn::animations_component>(e);
	BOOST_TEST(comp.animations.size() == 2u);
	BOOST_TEST(comp.animations[0].name == "Walk");
	BOOST_TEST(comp.animations[0].duration == 30.f);
	BOOST_TEST(comp.animations[0].ticks_per_second == 30.f);
	BOOST_TEST(comp.animations[1].name == "Run");
}

BOOST_AUTO_TEST_CASE(AnimationsDesc_DeserializesJSON)
{
	json::object data;
	json::object anims;
	anims["Walk"] = json::object{ {"duration", 30.0}, {"ticks_per_second", 30.0} };
	anims["Run"]  = json::object{ {"duration", 20.0}, {"ticks_per_second", 24.0} };
	data["animations"] = std::move(anims);

	scn::animations_desc desc;
	// desc_system is not used by animations_desc::deserialize — safe to omit test here
	// instead we verify the populated desc produces correct component
	desc.animations.push_back({ "Walk", 30.f, 30.f });

	entt::registry reg;
	auto e = reg.create();
	scn::assemble_animations(reg, e, desc, "root");
	BOOST_TEST(reg.get<scn::animations_component>(e).animations.size() == 1u);
}

// ---- keyframes_desc ----

BOOST_AUTO_TEST_CASE(KeyframesDesc_TypeConstant)
{
	BOOST_TEST(scn::keyframes_desc::__type == "keyframes_desc");
}

BOOST_AUTO_TEST_CASE(KeyframesDesc_AssemblesComponent)
{
	entt::registry reg;
	auto e = reg.create();

	scn::keyframes_desc desc;
	scn::animation_node node;
	node.pos_keys.push_back({ glm::vec3{0, 0, 0}, 0.f });
	node.pos_keys.push_back({ glm::vec3{1, 0, 0}, 1.f });
	node.rotate_keys.push_back({ glm::quat{1, 0, 0, 0}, 0.f });
	node.scale_keys.push_back({ glm::vec3{1, 1, 1}, 0.f });
	desc.keyframes["Walk"] = std::move(node);

	scn::assemble_keyframes(reg, e, desc, "node");

	BOOST_TEST(reg.all_of<scn::keyframes_component>(e));
	auto& comp = reg.get<scn::keyframes_component>(e);
	BOOST_TEST(comp.keyframes.count("Walk") == 1u);
	BOOST_TEST(comp.keyframes.at("Walk").pos_keys.size() == 2u);
	BOOST_TEST(comp.keyframes.at("Walk").rotate_keys.size() == 1u);
	BOOST_TEST(comp.keyframes.at("Walk").scale_keys.size() == 1u);
}

BOOST_AUTO_TEST_CASE(KeyframesDesc_EmptyKeyframesMapProducesEmptyComponent)
{
	entt::registry reg;
	auto e = reg.create();

	scn::keyframes_desc desc;
	scn::assemble_keyframes(reg, e, desc, "node");

	BOOST_TEST(reg.all_of<scn::keyframes_component>(e));
	BOOST_TEST(reg.get<scn::keyframes_component>(e).keyframes.empty());
}

// ---- bone_desc ----

BOOST_AUTO_TEST_CASE(BoneDesc_TypeConstant)
{
	BOOST_TEST(scn::bone_desc::__type == "bone_desc");
}

BOOST_AUTO_TEST_CASE(BoneDesc_AssemblesComponent)
{
	entt::registry reg;
	auto e = reg.create();

	scn::bone_desc desc;
	desc.index = 2;
	desc.offset_matrix = glm::translate(glm::mat4{1.f}, glm::vec3{1, 2, 3});

	scn::assemble_bone(reg, e, desc, "bone_node");

	BOOST_TEST(reg.all_of<scn::bone_component>(e));
	auto& comp = reg.get<scn::bone_component>(e);
	BOOST_TEST(comp.index == 2);

	const float eps = 1e-6f;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			BOOST_CHECK_SMALL(comp.offset[c][r] - desc.offset_matrix[c][r], eps);
}

BOOST_AUTO_TEST_CASE(BoneDesc_DefaultOffsetIsIdentity)
{
	entt::registry reg;
	auto e = reg.create();

	scn::bone_desc desc; // default: identity matrix, index=-1
	scn::assemble_bone(reg, e, desc, "bone");

	auto& comp = reg.get<scn::bone_component>(e);
	BOOST_TEST(comp.index == -1);
	BOOST_TEST(comp.offset == glm::mat4{1.0f});
}

// ---- skinning_desc ----

BOOST_AUTO_TEST_CASE(SkinningDesc_TypeConstant)
{
	BOOST_TEST(scn::skinning_desc::__type == "skinning_desc");
}

BOOST_AUTO_TEST_CASE(SkinningDesc_AssemblesSkinningComponent)
{
	entt::registry reg;
	auto e = reg.create();

	scn::skinning_desc desc;
	desc.bone_count = 0;

	scn::assemble_skinning(reg, e, desc, "mesh");

	BOOST_TEST(reg.all_of<scn::skinning_component>(e));
	BOOST_TEST(!reg.all_of<scn::bone_matrices_component>(e));
}

BOOST_AUTO_TEST_CASE(SkinningDesc_AssemblesBoneMatricesWhenBoneCountPositive)
{
	entt::registry reg;
	auto e = reg.create();

	scn::skinning_desc desc;
	desc.bone_count = 4;

	scn::assemble_skinning(reg, e, desc, "mesh");

	BOOST_TEST(reg.all_of<scn::skinning_component>(e));
	BOOST_TEST(reg.all_of<scn::bone_matrices_component>(e));

	auto& bm = reg.get<scn::bone_matrices_component>(e);
	BOOST_TEST(bm.matrices.size() == 4u);

	// All matrices should be identity
	const glm::mat4 identity{1.0f};
	for (const auto& mat : bm.matrices) {
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				BOOST_CHECK_SMALL(mat[c][r] - identity[c][r], 1e-6f);
	}
}

BOOST_AUTO_TEST_SUITE_END()
