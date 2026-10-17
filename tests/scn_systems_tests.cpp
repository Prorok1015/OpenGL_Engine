#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include "level/scn_level.h"
#include "scn_transform_system.h"
#include "scn_animation_job.h"
#include "scn_camera_controller_system.h"
#include "scn_camera_controller_component.hpp"
#include "scn_mesh_nodes.hpp"
#include "inp_events.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

// The REAL transform / animation / camera systems, scheduled by scn::level exactly like at
// runtime (no copies of the system code). Headless: only registries and a system_factory.

namespace {

using test_util::stat_component;

struct systems_fixture : test_util::scene_env
{
	systems_fixture() : lvl(nullptr)
	{
		scn::init_transform_system(sf);
		scn::init_animation_system(sf);
		scn::init_mouse_controller_system(sf);
	}

	scn::world& make_world(std::vector<std::string> systems)
	{
		auto& w = lvl.create_world("w", 0);
		lvl.add_world_systems(w, systems, sf);
		return w;
	}

	void step(float seconds) { lvl.update(std::chrono::duration<float>(seconds)); }

	// adds the components every hierarchy node needs and links it under `parent`
	static entt::entity node(entt::registry& reg, entt::entity parent, glm::vec3 local_pos)
	{
		auto e = reg.create();
		reg.emplace<scn::local_transform>(e, glm::translate(glm::mat4(1.0f), local_pos));
		reg.emplace<scn::world_transform>(e);
		reg.emplace<scn::depth_level>(e);
		if (parent != entt::null) {
			reg.emplace<scn::parent_component>(e, parent);
			if (auto* kids = reg.try_get<scn::children_component>(parent)) kids->children.push_back(e);
			else reg.emplace<scn::children_component>(parent, std::vector{ e });
		}
		return e;
	}

	static glm::vec3 world_pos(entt::registry& reg, entt::entity e) { return glm::vec3(reg.get<scn::world_transform>(e).world[3]); }

	static void emit_hierarchy(entt::registry& reg, entt::entity e) { reg.ctx().emplace<ecs::event<scn::hierarchy_updated>>().emit(e); }
	static void emit_transform(entt::registry& reg, entt::entity e) { reg.ctx().emplace<ecs::event<scn::transform_updated>>().emit(e); }

	scn::level lvl;
	ecs::system_factory sf;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnSystemsTests, systems_fixture)

// ---------------------------------------------------------------- depth + transform systems

BOOST_AUTO_TEST_CASE(DepthSystem_AssignsDepthFromParentChain)
{
	auto& reg = make_world({ "scn::depth_system" }).state();
	auto root = node(reg, entt::null, {});
	auto child = node(reg, root, {});
	auto grandchild = node(reg, child, {});
	emit_hierarchy(reg, root);

	step(0.016f);

	BOOST_TEST(reg.get<scn::depth_level>(root).value == 0u);
	BOOST_TEST(reg.get<scn::depth_level>(child).value == 1u);
	BOOST_TEST(reg.get<scn::depth_level>(grandchild).value == 2u);
	BOOST_TEST(reg.ctx().get<ecs::event<scn::hierarchy_updated>>().size() == 0u); // event consumed
}

BOOST_AUTO_TEST_CASE(DepthSystem_ReparentedSubtreeIsRecomputedFromTheEvent)
{
	auto& reg = make_world({ "scn::depth_system" }).state();
	auto a = node(reg, entt::null, {});
	auto b = node(reg, a, {});
	auto c = node(reg, b, {});
	emit_hierarchy(reg, a);
	step(0.016f);
	BOOST_REQUIRE(reg.get<scn::depth_level>(c).value == 2u);

	// move `c` directly under `a`
	auto& old_kids = reg.get<scn::children_component>(b).children;
	old_kids.clear();
	reg.get<scn::parent_component>(c).parent = a;
	reg.get<scn::children_component>(a).children.push_back(c);
	emit_hierarchy(reg, c);
	step(0.016f);

	BOOST_TEST(reg.get<scn::depth_level>(c).value == 1u);
}

BOOST_AUTO_TEST_CASE(DepthSystem_AddsDepthComponentWhenMissing)
{
	auto& reg = make_world({ "scn::depth_system" }).state();
	auto root = node(reg, entt::null, {});
	auto child = reg.create(); // no depth_level yet
	reg.emplace<scn::parent_component>(child, root);
	reg.emplace<scn::children_component>(root, std::vector{ child });
	emit_hierarchy(reg, root);
	step(0.016f);
	BOOST_REQUIRE(reg.all_of<scn::depth_level>(child));
	BOOST_TEST(reg.get<scn::depth_level>(child).value == 1u);
}

BOOST_AUTO_TEST_CASE(TransformSystem_WorldIsParentWorldTimesLocalDownTheWholeChain)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, { 1, 0, 0 });
	auto child = node(reg, root, { 0, 2, 0 });
	auto grandchild = node(reg, child, { 0, 0, 3 });
	emit_hierarchy(reg, root);
	for (auto e : { root, child, grandchild }) emit_transform(reg, e);

	step(0.016f);

	BOOST_TEST((world_pos(reg, root) == glm::vec3{ 1, 0, 0 }));
	BOOST_TEST((world_pos(reg, child) == glm::vec3{ 1, 2, 0 }));
	BOOST_TEST((world_pos(reg, grandchild) == glm::vec3{ 1, 2, 3 }));
	BOOST_TEST(reg.ctx().get<ecs::event<scn::transform_updated>>().size() == 0u);
}

BOOST_AUTO_TEST_CASE(TransformSystem_MovingParentPropagatesToDescendants)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, {});
	auto child = node(reg, root, { 0, 1, 0 });
	emit_hierarchy(reg, root);
	emit_transform(reg, root);
	step(0.016f);
	BOOST_REQUIRE((world_pos(reg, child) == glm::vec3{ 0, 1, 0 }));

	reg.get<scn::local_transform>(root).local = glm::translate(glm::mat4(1.0f), glm::vec3{ 10, 0, 0 });
	emit_transform(reg, root); // only the parent is reported
	step(0.016f);

	BOOST_TEST((world_pos(reg, child) == glm::vec3{ 10, 1, 0 }));
}

BOOST_AUTO_TEST_CASE(TransformSystem_ParentRotationAndScaleAffectChildPosition)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, {});
	reg.get<scn::local_transform>(root).local =
		glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3{ 0, 0, 1 }) * glm::scale(glm::mat4(1.0f), glm::vec3{ 2.0f });
	auto child = node(reg, root, { 1, 0, 0 });
	emit_hierarchy(reg, root);
	emit_transform(reg, root);
	step(0.016f);

	auto p = world_pos(reg, child);
	BOOST_TEST(std::abs(p.x) < 1e-5f);
	BOOST_TEST(p.y == 2.0f, boost::test_tools::tolerance(1e-5f)); // local +X scaled by 2 and rotated onto +Y
}

BOOST_AUTO_TEST_CASE(TransformSystem_ChildEventAloneUsesCurrentParentWorld)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, { 5, 0, 0 });
	auto child = node(reg, root, { 0, 1, 0 });
	emit_hierarchy(reg, root);
	emit_transform(reg, root);
	step(0.016f);

	reg.get<scn::local_transform>(child).local = glm::translate(glm::mat4(1.0f), glm::vec3{ 0, 7, 0 });
	emit_transform(reg, child);
	step(0.016f);
	BOOST_TEST((world_pos(reg, child) == glm::vec3{ 5, 7, 0 }));
}

// ---------------------------------------------------------------- animation systems

struct anim_fixture : systems_fixture
{
	anim_fixture()
	{
		auto coll = load<scn::animation_collection_desc>(put("anims.desc", R"({"__type":"animation_collection_desc","clips":[
			{"__type":"animation_clip_desc","name":"Walk","duration":10.0,"ticks_per_second":10.0,
			 "channels":{"Bone":{"position":[{"value":[0,0,0],"time":0.0},{"value":[10,0,0],"time":10.0}],"rotation":[],"scale":[]}}}]})"));
		collection = coll;
		auto& world = make_world({ "scn::animation_controller_update", "scn::animation_system_node",
			"scn::depth_system", "scn::transform_system", "scn::animation_system_matrix" });
		reg = &world.state();

		root = node(*reg, entt::null, {});
		bone = node(*reg, root, {});
		scn::animation_collection_component comp;
		comp.clips = coll->clips;
		reg->emplace<scn::animation_collection_component>(root, std::move(comp));
		auto& ctrl = reg->emplace<scn::animation_controller_component>(root);
		ctrl.current_animation = "Walk";
		ctrl.duration = 10.0f;
		ctrl.ticks_per_second = 10.0f;
		ctrl.playing = true;
		reg->emplace<scn::animated_node_component>(bone, scn::animated_node_component{ "Bone" });
		reg->emplace<scn::bone_component>(bone, scn::bone_component{ glm::translate(glm::mat4(1.0f), glm::vec3{ 0, 0, 1 }), 1 });
		reg->emplace<scn::bone_matrices_component>(root).matrices.resize(2, glm::mat4(0.0f));
		emit_hierarchy(*reg, root);
		emit_transform(*reg, root);
	}

	scn::animation_controller_component& ctrl() { return reg->get<scn::animation_controller_component>(root); }

	std::shared_ptr<scn::animation_collection_desc> collection;
	entt::registry* reg = nullptr;
	entt::entity root = entt::null;
	entt::entity bone = entt::null;
};

BOOST_FIXTURE_TEST_CASE(Anim_ControllerAdvancesTimeBySpeedTimesDelta, anim_fixture)
{
	ctrl().speed = 2.0f;
	step(0.25f);
	BOOST_TEST(ctrl().current_time == 0.5f, boost::test_tools::tolerance(1e-5f));
	step(0.25f);
	BOOST_TEST(ctrl().current_time == 1.0f, boost::test_tools::tolerance(1e-5f)); // exactly the end: not past it yet
}

BOOST_FIXTURE_TEST_CASE(Anim_PausedControllerDoesNotAdvance, anim_fixture)
{
	ctrl().playing = false;
	step(0.5f);
	BOOST_TEST(ctrl().current_time == 0.0f);
}

BOOST_FIXTURE_TEST_CASE(Anim_LoopingControllerWrapsToZeroPastTheEnd, anim_fixture)
{
	step(1.2f); // 12 ticks > 10
	BOOST_TEST(ctrl().current_time == 0.0f);
	BOOST_TEST(ctrl().playing);
}

BOOST_FIXTURE_TEST_CASE(Anim_NonLoopingControllerStopsAtTheEnd, anim_fixture)
{
	ctrl().loop = false;
	step(1.5f);
	BOOST_TEST(ctrl().current_time == 1.0f, boost::test_tools::tolerance(1e-5f)); // duration / tps
	BOOST_TEST(!ctrl().playing);
}

BOOST_FIXTURE_TEST_CASE(Anim_NodeSystemInterpolatesChannelIntoLocalAndWorldTransform, anim_fixture)
{
	step(0.5f); // 5 ticks -> halfway along the 0..10 position track
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(bone).local[3]) == glm::vec3{ 5, 0, 0 }));
	BOOST_TEST((world_pos(*reg, bone) == glm::vec3{ 5, 0, 0 })); // transform system ran after it in the same frame
}

BOOST_FIXTURE_TEST_CASE(Anim_BoneMatrixIsWorldTimesOffsetAtItsIndex, anim_fixture)
{
	step(0.5f);
	const auto& m = reg->get<scn::bone_matrices_component>(root).matrices;
	BOOST_REQUIRE(m.size() == 2u);
	BOOST_TEST((glm::vec3(m[1][3]) == glm::vec3{ 5, 0, 1 })); // world (5,0,0) * offset translate(0,0,1)
	BOOST_TEST(m[0][3][3] == 0.0f);                           // index 0 has no bone: untouched
}

BOOST_FIXTURE_TEST_CASE(Anim_BoneWithOutOfRangeIndexIsIgnored, anim_fixture)
{
	reg->get<scn::bone_component>(bone).index = 7;
	BOOST_CHECK_NO_THROW(step(0.5f));
	BOOST_TEST(reg->get<scn::bone_matrices_component>(root).matrices.size() == 2u);
}

BOOST_FIXTURE_TEST_CASE(Anim_UnknownClipNameLeavesNodesUntouched, anim_fixture)
{
	ctrl().current_animation = "Run";
	step(0.5f);
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(bone).local[3]) == glm::vec3{ 0, 0, 0 }));
}

BOOST_FIXTURE_TEST_CASE(Anim_NodeWithoutChannelInClipIsSkipped, anim_fixture)
{
	reg->get<scn::animated_node_component>(bone).node_name = "Other";
	step(0.5f);
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(bone).local[3]) == glm::vec3{ 0, 0, 0 }));
}

// ---------------------------------------------------------------- camera controller

struct camera_fixture : systems_fixture
{
	camera_fixture()
	{
		auto& world = make_world({ "scn::update_camera_matrix_system", "inp::clear_input_state" });
		reg = &world.state();
		state = &reg->ctx().emplace<inp::input_state>();
		reg->ctx().emplace<inp::input_focus>();
		cam = reg->create();
		reg->emplace<scn::local_transform>(cam);
		reg->emplace<scn::mouse_controller_component>(cam);
	}

	scn::mouse_controller_component& ctl() { return reg->get<scn::mouse_controller_component>(cam); }
	std::size_t events() { return reg->ctx().emplace<ecs::event<scn::transform_updated>>().size(); }
	glm::vec3 cam_pos() { return glm::vec3(reg->get<scn::local_transform>(cam).local[3]); }

	entt::registry* reg = nullptr;
	inp::input_state* state = nullptr;
	entt::entity cam = entt::null;
};

BOOST_FIXTURE_TEST_CASE(Camera_LeftDragPansAlongGroundAndEmitsTransformUpdate, camera_fixture)
{
	state->mouse[(int)inp::MOUSE_BUTTONS::LEFT] = true;
	state->mouse_dir = { 0.1f, 0.2f };
	step(0.016f);

	BOOST_TEST(ctl().is_moving);
	BOOST_TEST(ctl().position.x == 0.3f, boost::test_tools::tolerance(1e-5f));  // dir.x * speed(3)
	BOOST_TEST(ctl().position.z == 0.6f, boost::test_tools::tolerance(1e-5f));  // dir.y -> world Z
	BOOST_TEST(cam_pos().z == 0.6f + 2.0f, boost::test_tools::tolerance(1e-5f)); // + orbit distance 2 along local Z
	BOOST_TEST(events() == 1u);
}

BOOST_FIXTURE_TEST_CASE(Camera_RightDragRotatesAndClampsPitch, camera_fixture)
{
	state->mouse[(int)inp::MOUSE_BUTTONS::RIGHT] = true;
	state->mouse_dir = { 0.5f, 0.25f };
	step(0.016f);
	BOOST_TEST(ctl().rotation.y == 0.5f * glm::radians(90.0f), boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(ctl().rotation.x == 0.25f * glm::radians(90.0f), boost::test_tools::tolerance(1e-5f));

	state->mouse_dir = { 0.0f, 100.0f }; // absurd drag
	step(0.016f);
	BOOST_TEST(ctl().rotation.x == glm::radians(90.0f), boost::test_tools::tolerance(1e-5f)); // pitch clamped
}

BOOST_FIXTURE_TEST_CASE(Camera_PanFollowsCurrentYaw, camera_fixture)
{
	ctl().rotation.y = glm::radians(90.0f);
	state->mouse[(int)inp::MOUSE_BUTTONS::LEFT] = true;
	state->mouse_dir = { 1.0f, 0.0f };
	step(0.016f);
	// +X drag rotated by 90 degrees about Y ends up along -Z
	BOOST_TEST(std::abs(ctl().position.x) < 1e-5f);
	BOOST_TEST(ctl().position.z == -3.0f, boost::test_tools::tolerance(1e-5f));
}

BOOST_FIXTURE_TEST_CASE(Camera_ScrollUpZoomsInAndClampsAtZero, camera_fixture)
{
	state->scroll = { 0.0f, 1.0f };
	step(0.016f);
	BOOST_TEST(ctl().distance == 0.0f); // 2 - 1*3 clamped to 0
	BOOST_TEST(events() == 1u);
}

// BUG: scn_camera_controller_system.cpp -- the transform_updated event is only emitted when
// `state.scroll.y > 0`, so scrolling OUT (negative scroll) changes `distance` and the local matrix
// but never tells the transform system, leaving the camera's world matrix stale until another
// input arrives.
BOOST_TEST_DECORATOR(*boost::unit_test::expected_failures(1))
BOOST_FIXTURE_TEST_CASE(Camera_ScrollDownZoomOutNotifiesTransformSystem, camera_fixture)
{
	state->scroll = { 0.0f, -1.0f };
	step(0.016f);
	BOOST_TEST(ctl().distance == 5.0f);   // 2 + 3 (this part works)
	BOOST_TEST(events() == 1u);           // but no event was emitted
}

BOOST_FIXTURE_TEST_CASE(Camera_IdleFrameRebuildsMatrixWithoutEvent, camera_fixture)
{
	step(0.016f);
	BOOST_TEST(events() == 0u);
	BOOST_TEST(cam_pos().z == 2.0f, boost::test_tools::tolerance(1e-5f));
}

BOOST_FIXTURE_TEST_CASE(Camera_InactiveFocusIgnoresInput, camera_fixture)
{
	reg->ctx().get<inp::input_focus>().active = false;
	state->mouse[(int)inp::MOUSE_BUTTONS::LEFT] = true;
	state->mouse_dir = { 1.0f, 1.0f };
	step(0.016f);
	BOOST_TEST(ctl().position.x == 0.0f);
	BOOST_TEST(events() == 0u);
}

BOOST_FIXTURE_TEST_CASE(Camera_ClearInputSystemResetsFrameDeltasButKeepsButtons, camera_fixture)
{
	state->mouse[(int)inp::MOUSE_BUTTONS::LEFT] = true;
	state->scroll = { 0, 1 };
	state->mouse_dir = { 1, 1 };
	step(0.016f);
	BOOST_TEST((state->scroll == glm::vec2{ 0 }));
	BOOST_TEST((state->mouse_dir == glm::vec2{ 0 }));
	BOOST_TEST(state->mouse[(int)inp::MOUSE_BUTTONS::LEFT]);
}

BOOST_AUTO_TEST_SUITE_END()
