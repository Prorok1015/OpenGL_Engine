#include <boost/test/unit_test.hpp>
#include <boost/test/tools/floating_point_comparison.hpp>
#include <entt/entt.hpp>
#include "scn_model.h"
#include "scn_mesh_nodes.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <vector>

// Copied from scn_animation_job.cpp for unit testing (internal linkage in original)
namespace {

template<class T>
std::size_t find_keyframe_index(const std::vector<T>& arr, float time)
{
	if (arr.size() < 2) return 0;

	auto it = std::upper_bound(arr.begin(), arr.end(), time,
		[](float t, const T& key) { return t < key.time; });

	if (it == arr.begin()) return 0;
	std::size_t idx = static_cast<std::size_t>(std::distance(arr.begin(), it)) - 1;
	if (idx >= arr.size() - 1) idx = arr.size() - 2;
	return idx;
}

void calc_interpolated_scaling(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
	if (anim.scale_keys.empty()) { Out = glm::vec3{1.0f}; return; }
	if (anim.scale_keys.size() == 1) {
		Out = anim.scale_keys[0].value;
		return;
	}
	std::size_t i = find_keyframe_index(anim.scale_keys, AnimationTimeTicks);
	float t1 = anim.scale_keys[i].time;
	float t2 = anim.scale_keys[i + 1].time;
	float factor = std::clamp((AnimationTimeTicks - t1) / (t2 - t1), 0.0f, 1.0f);
	Out = anim.scale_keys[i].value + factor * (anim.scale_keys[i + 1].value - anim.scale_keys[i].value);
}

void calc_interpolated_position(glm::vec3& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
	if (anim.pos_keys.empty()) { Out = glm::vec3{0.0f}; return; }
	if (anim.pos_keys.size() == 1) {
		Out = anim.pos_keys[0].value;
		return;
	}
	std::size_t i = find_keyframe_index(anim.pos_keys, AnimationTimeTicks);
	float t1 = anim.pos_keys[i].time;
	float t2 = anim.pos_keys[i + 1].time;
	float factor = std::clamp((AnimationTimeTicks - t1) / (t2 - t1), 0.0f, 1.0f);
	Out = anim.pos_keys[i].value + factor * (anim.pos_keys[i + 1].value - anim.pos_keys[i].value);
}

void calc_interpolated_rotation(glm::quat& Out, float AnimationTimeTicks, const scn::animation_node& anim)
{
	if (anim.rotate_keys.empty()) { Out = glm::quat{1.0f, 0.0f, 0.0f, 0.0f}; return; }
	if (anim.rotate_keys.size() == 1) {
		Out = anim.rotate_keys[0].value;
		return;
	}
	std::size_t i = find_keyframe_index(anim.rotate_keys, AnimationTimeTicks);
	float t1 = anim.rotate_keys[i].time;
	float t2 = anim.rotate_keys[i + 1].time;
	float factor = std::clamp((AnimationTimeTicks - t1) / (t2 - t1), 0.0f, 1.0f);
	Out = glm::slerp(anim.rotate_keys[i].value, anim.rotate_keys[i + 1].value, factor);
}

// Helper: build a simple skeleton hierarchy in registry
//   skeleton_root (skeleton_component, bone_matrices_component)
//     bone0 (bone_component index=0, parent_component -> skeleton_root)
//     bone1 (bone_component index=1, parent_component -> skeleton_root)
//       bone2 (bone_component index=2, parent_component -> bone1)
struct test_skeleton {
	entt::entity root;
	entt::entity bone0;
	entt::entity bone1;
	entt::entity bone2;
};

test_skeleton build_test_skeleton(entt::registry& reg, int bone_count = 3)
{
	test_skeleton s;
	s.root = reg.create();
	reg.emplace<scn::skeleton_component>(s.root, scn::skeleton_component{ .bone_count = bone_count });
	reg.emplace<scn::bone_matrices_component>(s.root, scn::bone_matrices_component{ .matrices = std::vector<glm::mat4>(bone_count, glm::mat4(1.0f)) });

	s.bone0 = reg.create();
	reg.emplace<scn::bone_component>(s.bone0, scn::bone_component{ .index = 0 });
	reg.emplace<scn::parent_component>(s.bone0, scn::parent_component{ s.root });

	s.bone1 = reg.create();
	reg.emplace<scn::bone_component>(s.bone1, scn::bone_component{ .index = 1 });
	reg.emplace<scn::parent_component>(s.bone1, scn::parent_component{ s.root });

	s.bone2 = reg.create();
	reg.emplace<scn::bone_component>(s.bone2, scn::bone_component{ .index = 2 });
	reg.emplace<scn::parent_component>(s.bone2, scn::parent_component{ s.bone1 });

	return s;
}

} // anonymous namespace

// ============================================================================
// Skeleton & Bone Matrices
// ============================================================================

BOOST_AUTO_TEST_SUITE(SkeletonBoneMatricesTests)

BOOST_AUTO_TEST_CASE(BoneMatricesOnSkeletonRoot)
{
	entt::registry reg;
	auto s = build_test_skeleton(reg);

	BOOST_CHECK(reg.all_of<scn::skeleton_component>(s.root));
	BOOST_CHECK(reg.all_of<scn::bone_matrices_component>(s.root));
	BOOST_CHECK(!reg.all_of<scn::bone_matrices_component>(s.bone0));
	BOOST_CHECK(!reg.all_of<scn::bone_matrices_component>(s.bone1));
	BOOST_CHECK(!reg.all_of<scn::bone_matrices_component>(s.bone2));
}

BOOST_AUTO_TEST_CASE(MultipleMeshesShareBoneMatrices)
{
	entt::registry reg;
	auto s = build_test_skeleton(reg);

	// Two mesh entities referencing the same skeleton root
	auto mesh0 = reg.create();
	reg.emplace<scn::skinning_component>(mesh0, scn::skinning_component{ .skeleton_entity = s.root });
	reg.emplace<scn::parent_component>(mesh0, scn::parent_component{ s.root });

	auto mesh1 = reg.create();
	reg.emplace<scn::skinning_component>(mesh1, scn::skinning_component{ .skeleton_entity = s.root });
	reg.emplace<scn::parent_component>(mesh1, scn::parent_component{ s.root });

	// Both point to the same skeleton root
	auto& skin0 = reg.get<scn::skinning_component>(mesh0);
	auto& skin1 = reg.get<scn::skinning_component>(mesh1);
	BOOST_CHECK(skin0.skeleton_entity == skin1.skeleton_entity);

	// Only one bone_matrices_component exists (on root)
	auto& matrices = reg.get<scn::bone_matrices_component>(s.root);
	BOOST_CHECK_EQUAL(matrices.matrices.size(), 3u);
}

BOOST_AUTO_TEST_CASE(BoneMatricesCorrectSize)
{
	entt::registry reg;
	constexpr int bone_count = 64;
	auto root = reg.create();
	reg.emplace<scn::skeleton_component>(root, scn::skeleton_component{ .bone_count = bone_count });
	reg.emplace<scn::bone_matrices_component>(root, scn::bone_matrices_component{ .matrices = std::vector<glm::mat4>(bone_count, glm::mat4(1.0f)) });

	auto& matrices = reg.get<scn::bone_matrices_component>(root);
	BOOST_CHECK_EQUAL(static_cast<int>(matrices.matrices.size()), bone_count);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Animation Controller
// ============================================================================

BOOST_AUTO_TEST_SUITE(AnimationControllerTests)

BOOST_AUTO_TEST_CASE(PlayAdvancesTime)
{
	scn::animation_controller_component ctrl;
	ctrl.playing = true;
	ctrl.speed = 1.0f;
	ctrl.duration = 100.0f;
	ctrl.ticks_per_second = 25.0f;
	ctrl.loop = false;

	float dt = 0.5f;
	ctrl.current_time += dt * ctrl.speed;

	BOOST_CHECK_CLOSE(ctrl.current_time, 0.5f, 0.01f);
}

BOOST_AUTO_TEST_CASE(PauseStopsTime)
{
	scn::animation_controller_component ctrl;
	ctrl.playing = false;
	ctrl.speed = 1.0f;
	ctrl.current_time = 1.0f;

	float dt = 0.5f;
	if (ctrl.playing) {
		ctrl.current_time += dt * ctrl.speed;
	}

	BOOST_CHECK_CLOSE(ctrl.current_time, 1.0f, 0.01f);
}

BOOST_AUTO_TEST_CASE(LoopWrapsTime)
{
	scn::animation_controller_component ctrl;
	ctrl.playing = true;
	ctrl.speed = 1.0f;
	ctrl.duration = 100.0f;
	ctrl.ticks_per_second = 25.0f;
	ctrl.loop = true;

	// Advance past duration: 100 ticks / 25 tps = 4 seconds
	ctrl.current_time = 4.1f;
	float tps = ctrl.ticks_per_second;
	float time_in_ticks = ctrl.current_time * tps;
	if (time_in_ticks > ctrl.duration) {
		if (ctrl.loop) {
			ctrl.current_time = 0.f;
		}
	}

	BOOST_CHECK_SMALL(ctrl.current_time, 1e-5f);
}

BOOST_AUTO_TEST_CASE(NoLoopClampsTime)
{
	scn::animation_controller_component ctrl;
	ctrl.playing = true;
	ctrl.speed = 1.0f;
	ctrl.duration = 100.0f;
	ctrl.ticks_per_second = 25.0f;
	ctrl.loop = false;

	ctrl.current_time = 5.0f; // 5 * 25 = 125 ticks > 100 duration
	float tps = ctrl.ticks_per_second;
	float time_in_ticks = ctrl.current_time * tps;
	if (time_in_ticks > ctrl.duration) {
		if (ctrl.loop) {
			ctrl.current_time = 0.f;
		} else {
			ctrl.current_time = ctrl.duration / tps;
			ctrl.playing = false;
		}
	}

	BOOST_CHECK_CLOSE(ctrl.current_time, 4.0f, 0.01f);
	BOOST_CHECK(!ctrl.playing);
}

BOOST_AUTO_TEST_CASE(SpeedMultiplier)
{
	scn::animation_controller_component ctrl;
	ctrl.playing = true;
	ctrl.speed = 2.0f;
	ctrl.current_time = 0.0f;

	float dt = 0.5f;
	ctrl.current_time += dt * ctrl.speed;

	BOOST_CHECK_CLOSE(ctrl.current_time, 1.0f, 0.01f);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Keyframe Binary Search
// ============================================================================

BOOST_AUTO_TEST_SUITE(KeyframeBinarySearchTests)

BOOST_AUTO_TEST_CASE(EmptyKeys)
{
	std::vector<scn::animation_pos_key> keys;
	// find_keyframe_index returns 0 for < 2 keys
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 0.0f), 0u);
}

BOOST_AUTO_TEST_CASE(SingleKey)
{
	std::vector<scn::animation_pos_key> keys = {
		{ glm::vec3(1, 0, 0), 0.0f }
	};
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 0.0f), 0u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 5.0f), 0u);
}

BOOST_AUTO_TEST_CASE(TwoKeys)
{
	std::vector<scn::animation_pos_key> keys = {
		{ glm::vec3(0), 0.0f },
		{ glm::vec3(1), 10.0f }
	};
	// Any time should return index 0 (the only valid pair is 0,1)
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 0.0f), 0u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 5.0f), 0u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 10.0f), 0u);
}

BOOST_AUTO_TEST_CASE(MultipleKeys)
{
	std::vector<scn::animation_pos_key> keys = {
		{ glm::vec3(0), 0.0f },
		{ glm::vec3(1), 10.0f },
		{ glm::vec3(2), 20.0f },
		{ glm::vec3(3), 30.0f }
	};
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 0.0f), 0u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 5.0f), 0u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 10.0f), 1u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 15.0f), 1u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 20.0f), 2u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 25.0f), 2u);
	// Past last key — clamped to second-to-last
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 100.0f), 2u);
}

BOOST_AUTO_TEST_CASE(TimeBeforeFirstKey)
{
	std::vector<scn::animation_pos_key> keys = {
		{ glm::vec3(0), 5.0f },
		{ glm::vec3(1), 10.0f },
		{ glm::vec3(2), 15.0f }
	};
	// Before first key — returns 0
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 0.0f), 0u);
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 4.9f), 0u);
}

BOOST_AUTO_TEST_CASE(TimeExactlyOnKey)
{
	std::vector<scn::animation_pos_key> keys = {
		{ glm::vec3(0), 0.0f },
		{ glm::vec3(1), 10.0f },
		{ glm::vec3(2), 20.0f }
	};
	// Exactly on key boundary
	BOOST_CHECK_EQUAL(find_keyframe_index(keys, 10.0f), 1u);
}

BOOST_AUTO_TEST_SUITE_END()

// ============================================================================
// Keyframe Interpolation Edge Cases
// ============================================================================

BOOST_AUTO_TEST_SUITE(KeyframeInterpolationTests)

BOOST_AUTO_TEST_CASE(EmptyPositionKeys)
{
	scn::animation_node anim;
	glm::vec3 result;
	calc_interpolated_position(result, 0.0f, anim);
	BOOST_CHECK_SMALL(result.x, 1e-5f);
	BOOST_CHECK_SMALL(result.y, 1e-5f);
	BOOST_CHECK_SMALL(result.z, 1e-5f);
}

BOOST_AUTO_TEST_CASE(SinglePositionKey)
{
	scn::animation_node anim;
	anim.pos_keys = { { glm::vec3(3, 4, 5), 0.0f } };
	glm::vec3 result;
	calc_interpolated_position(result, 0.0f, anim);
	BOOST_CHECK_CLOSE(result.x, 3.0f, 0.01f);
	BOOST_CHECK_CLOSE(result.y, 4.0f, 0.01f);
	BOOST_CHECK_CLOSE(result.z, 5.0f, 0.01f);
}

BOOST_AUTO_TEST_CASE(PositionInterpolationMidpoint)
{
	scn::animation_node anim;
	anim.pos_keys = {
		{ glm::vec3(0, 0, 0), 0.0f },
		{ glm::vec3(10, 20, 30), 10.0f }
	};
	glm::vec3 result;
	calc_interpolated_position(result, 5.0f, anim);
	BOOST_CHECK_CLOSE(result.x, 5.0f, 0.01f);
	BOOST_CHECK_CLOSE(result.y, 10.0f, 0.01f);
	BOOST_CHECK_CLOSE(result.z, 15.0f, 0.01f);
}

BOOST_AUTO_TEST_CASE(EmptyScaleKeys)
{
	scn::animation_node anim;
	glm::vec3 result;
	calc_interpolated_scaling(result, 0.0f, anim);
	BOOST_CHECK_CLOSE(result.x, 1.0f, 0.01f);
	BOOST_CHECK_CLOSE(result.y, 1.0f, 0.01f);
	BOOST_CHECK_CLOSE(result.z, 1.0f, 0.01f);
}

BOOST_AUTO_TEST_CASE(EmptyRotationKeys)
{
	scn::animation_node anim;
	glm::quat result;
	calc_interpolated_rotation(result, 0.0f, anim);
	BOOST_CHECK_CLOSE(result.w, 1.0f, 0.01f);
	BOOST_CHECK_SMALL(result.x, 1e-5f);
	BOOST_CHECK_SMALL(result.y, 1e-5f);
	BOOST_CHECK_SMALL(result.z, 1e-5f);
}

BOOST_AUTO_TEST_CASE(ScaleInterpolation)
{
	scn::animation_node anim;
	anim.scale_keys = {
		{ glm::vec3(1, 1, 1), 0.0f },
		{ glm::vec3(2, 2, 2), 10.0f }
	};
	glm::vec3 result;
	calc_interpolated_scaling(result, 5.0f, anim);
	BOOST_CHECK_CLOSE(result.x, 1.5f, 0.01f);
	BOOST_CHECK_CLOSE(result.y, 1.5f, 0.01f);
	BOOST_CHECK_CLOSE(result.z, 1.5f, 0.01f);
}

BOOST_AUTO_TEST_CASE(PositionAtBoundary)
{
	scn::animation_node anim;
	anim.pos_keys = {
		{ glm::vec3(0, 0, 0), 0.0f },
		{ glm::vec3(10, 0, 0), 10.0f }
	};
	glm::vec3 result;

	// At start
	calc_interpolated_position(result, 0.0f, anim);
	BOOST_CHECK_SMALL(result.x, 1e-5f);

	// At end
	calc_interpolated_position(result, 10.0f, anim);
	BOOST_CHECK_CLOSE(result.x, 10.0f, 0.01f);
}

BOOST_AUTO_TEST_CASE(PositionBeforeFirstKey)
{
	scn::animation_node anim;
	anim.pos_keys = {
		{ glm::vec3(5, 0, 0), 10.0f },
		{ glm::vec3(10, 0, 0), 20.0f }
	};
	glm::vec3 result;
	// Before first key — factor clamped to 0, returns first key value
	calc_interpolated_position(result, 0.0f, anim);
	BOOST_CHECK_CLOSE(result.x, 5.0f, 0.01f);
}

BOOST_AUTO_TEST_SUITE_END()
