#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/scn_transform_system.h>
#include <snk/scene/scn_animation_job.h>
#include <snk/scene/scn_camera_controller_system.h>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_mesh_nodes.hpp>
#include <snk/input/inp_events.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cmath>

// The REAL transform / animation / camera systems, scheduled by scn::context_set exactly like at
// runtime (no copies of the system code). Headless: only registries and a system_factory.

namespace {

using test_util::stat_component;

// Stands in for game code that despawns a prefab tree mid-frame with a bare reg.destroy (#543).
struct despawn_request { entt::entity root = entt::null; };

void destroy_tree(entt::registry& reg, entt::entity e)
{
	if (auto* kids = reg.try_get<scn::children_component>(e)) {
		for (auto child : std::vector(kids->children)) destroy_tree(reg, child);
	}
	reg.destroy(e);
}

void despawn_system(entt::registry& reg)
{
	auto* request = reg.ctx().find<despawn_request>();
	if (!request || request->root == entt::null) return;
	destroy_tree(reg, request->root);
	request->root = entt::null;
}

struct systems_fixture : test_util::scene_env
{
	systems_fixture() : lvl("level", nullptr)
	{
		scn::init_transform_system(sf);
		scn::init_animation_system(sf);
		scn::init_mouse_controller_system(sf);
		sf.register_system<&despawn_system>("test::despawn",
			{ .phase = ecs::phase::update, .after = { "scn::animation_system_node" }, .run_in_edit = true });
	}

	scn::world_context& make_world(std::vector<std::string> systems)
	{
		auto& w = lvl.create_context("w", 0);
		lvl.add_systems(w, systems, sf);
		return w;
	}

	void step(float seconds) { lvl.update(std::chrono::duration<float>(seconds)); }

	// The level clamps one frame's delta to 0.25 s; longer spans are played as several frames.
	void run_for(float seconds)
	{
		while (seconds > 0.0f) {
			const float frame = std::min(seconds, 0.25f);
			step(frame);
			seconds -= frame;
		}
	}

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

	scn::context_set lvl;
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

// ---------------------------------------------------------------- entities destroyed before phase::transform (#543)

BOOST_AUTO_TEST_CASE(TransformSystem_SkipsEntityDestroyedAfterItsEvent)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, { 1, 0, 0 });
	auto child = node(reg, root, { 0, 1, 0 });
	auto other = node(reg, entt::null, { 0, 0, 4 });
	emit_hierarchy(reg, root);
	emit_hierarchy(reg, other);
	step(0.016f);

	emit_transform(reg, child);
	emit_transform(reg, other);
	reg.destroy(child);
	step(0.016f);

	BOOST_TEST((world_pos(reg, other) == glm::vec3{ 0, 0, 4 }));
	BOOST_TEST(reg.ctx().get<ecs::event<scn::transform_updated>>().size() == 0u);
}

BOOST_AUTO_TEST_CASE(DepthSystem_SkipsEntityDestroyedAfterItsEvent)
{
	auto& reg = make_world({ "scn::depth_system" }).state();
	auto gone = reg.create(); // no depth_level yet: the system would add one
	auto kept = node(reg, entt::null, {});
	emit_hierarchy(reg, gone);
	emit_hierarchy(reg, kept);
	reg.destroy(gone);
	step(0.016f);

	BOOST_TEST(!reg.storage<scn::depth_level>().contains(gone)); // no component left behind for a dead id
	BOOST_TEST(reg.get<scn::depth_level>(kept).value == 0u);
}

BOOST_AUTO_TEST_CASE(TransformSystem_SkipsAndUnlinksDestroyedChildStillListedByItsParent)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, { 1, 0, 0 });
	auto gone = node(reg, root, { 0, 1, 0 });
	auto kept = node(reg, root, { 0, 2, 0 });
	emit_hierarchy(reg, root);
	step(0.016f);

	reg.destroy(gone); // bare destroy: `root` still lists it
	emit_transform(reg, root);
	step(0.016f);

	BOOST_TEST((world_pos(reg, kept) == glm::vec3{ 1, 2, 0 }));
	BOOST_TEST((reg.get<scn::children_component>(root).children == std::vector{ kept }));
}

BOOST_AUTO_TEST_CASE(DepthSystem_SkipsAndUnlinksDestroyedChildStillListedByItsParent)
{
	auto& reg = make_world({ "scn::depth_system" }).state();
	auto root = node(reg, entt::null, {});
	auto gone = node(reg, root, {});
	auto kept = node(reg, root, {});
	reg.destroy(gone);
	emit_hierarchy(reg, root);
	step(0.016f);

	BOOST_TEST(reg.get<scn::depth_level>(kept).value == 1u);
	BOOST_TEST((reg.get<scn::children_component>(root).children == std::vector{ kept }));
}

BOOST_AUTO_TEST_CASE(TransformSystem_StaleChildHandleDoesNotReachTheEntityThatReusedItsIndex)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, { 1, 0, 0 });
	auto gone = node(reg, root, { 0, 1, 0 });
	emit_hierarchy(reg, root);
	step(0.016f);

	reg.destroy(gone); // bare destroy: `root` still lists the old handle
	auto fresh = reg.create();
	BOOST_REQUIRE(entt::to_entity(fresh) == entt::to_entity(gone)); // EnTT hands the index out again
	const glm::mat4 untouched = glm::translate(glm::mat4(1.0f), glm::vec3{ 7, 7, 7 });
	reg.emplace<scn::local_transform>(fresh, glm::translate(glm::mat4(1.0f), glm::vec3{ 0, 5, 0 }));
	reg.emplace<scn::world_transform>(fresh).world = untouched;
	emit_hierarchy(reg, root);
	emit_transform(reg, root);
	step(0.016f);

	BOOST_TEST((reg.get<scn::world_transform>(fresh).world == untouched)); // not treated as root's child
	BOOST_TEST(reg.get<scn::children_component>(root).children.empty());
	BOOST_TEST(!reg.all_of<scn::depth_level>(fresh));
}

BOOST_AUTO_TEST_CASE(TransformSystem_KeepsLiveChildWithoutTransformInItsParentsList)
{
	auto& reg = make_world({ "scn::depth_system", "scn::transform_system" }).state();
	auto root = node(reg, entt::null, {});
	auto bare = reg.create(); // alive, but no local/world transform
	reg.emplace<scn::parent_component>(bare, root);
	reg.emplace<scn::children_component>(root, std::vector{ bare });
	emit_hierarchy(reg, root);
	emit_transform(reg, root);
	step(0.016f);

	BOOST_TEST((reg.get<scn::children_component>(root).children == std::vector{ bare }));
}

// ---------------------------------------------------------------- animation systems

struct anim_fixture : systems_fixture
{
	anim_fixture() : anim_fixture(false) {}

	explicit anim_fixture(bool with_despawn)
	{
		auto coll = load<scn::animation_collection_desc>(put("anims.desc", R"({"__type":"animation_collection_desc","clips":[
			{"__type":"animation_clip_desc","name":"Walk","duration":10.0,"ticks_per_second":10.0,
			 "channels":{"Bone":{"position":[{"value":[0,0,0],"time":0.0},{"value":[10,0,0],"time":10.0}],"rotation":[],"scale":[]}}}]})"));
		collection = coll;
		std::vector<std::string> systems{ "scn::animation_controller_update", "scn::animation_system_node",
			"scn::depth_system", "scn::transform_system", "scn::animation_system_matrix" };
		if (with_despawn) systems.insert(systems.begin() + 2, "test::despawn");
		auto& world = make_world(std::move(systems));
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
	run_for(1.2f); // 12 ticks > 10
	BOOST_TEST(ctrl().current_time == 0.0f);
	BOOST_TEST(ctrl().playing);
}

BOOST_FIXTURE_TEST_CASE(Anim_NonLoopingControllerStopsAtTheEnd, anim_fixture)
{
	ctrl().loop = false;
	run_for(1.5f);
	BOOST_TEST(ctrl().current_time == 1.0f, boost::test_tools::tolerance(1e-5f)); // duration / tps
	BOOST_TEST(!ctrl().playing);
}

BOOST_FIXTURE_TEST_CASE(Anim_NodeSystemInterpolatesChannelIntoLocalAndWorldTransform, anim_fixture)
{
	run_for(0.5f); // 5 ticks -> halfway along the 0..10 position track
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(bone).local[3]) == glm::vec3{ 5, 0, 0 }));
	BOOST_TEST((world_pos(*reg, bone) == glm::vec3{ 5, 0, 0 })); // transform system ran after it in the same frame
}

BOOST_FIXTURE_TEST_CASE(Anim_BoneMatrixIsWorldTimesOffsetAtItsIndex, anim_fixture)
{
	run_for(0.5f);
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

// #742: the clip is looked up once per skeleton root in a pass; each root keeps its own answer, a missing clip included.
BOOST_AUTO_TEST_CASE(Anim_ClipPerRoot_LooksUpOncePerRoot)
{
	entt::registry r;
	const entt::entity a = r.create();
	const entt::entity b = r.create();
	const int clip_a = 1;
	int lookups = 0;
	scn::clip_per_root<int> clips(2);
	const auto found = [&](const int* clip) { return [&lookups, clip] { ++lookups; return clip; }; };
	BOOST_TEST(clips.get(a, found(&clip_a)) == &clip_a);
	BOOST_TEST(clips.get(a, found(nullptr)) == &clip_a);
	BOOST_TEST(lookups == 1);
	BOOST_TEST(clips.get(b, found(nullptr)) == nullptr);
	BOOST_TEST(clips.get(b, found(&clip_a)) == nullptr);
	BOOST_TEST(lookups == 2);
}

// Two nodes of one skeleton play its clip; a second skeleton whose clip is missing leaves its node alone.
BOOST_FIXTURE_TEST_CASE(Anim_NodesOfOneSkeletonShareItsClip_AnotherSkeletonKeepsItsOwn, anim_fixture)
{
	const entt::entity twin = node(*reg, root, {});
	reg->emplace<scn::animated_node_component>(twin, scn::animated_node_component{ "Bone" });
	const entt::entity other_root = node(*reg, entt::null, {});
	const entt::entity other_bone = node(*reg, other_root, {});
	scn::animation_collection_component other_clips;
	other_clips.clips = collection->clips;
	reg->emplace<scn::animation_collection_component>(other_root, std::move(other_clips));
	auto& other = reg->emplace<scn::animation_controller_component>(other_root);
	other.current_animation = "Run";
	other.duration = 10.0f;
	other.ticks_per_second = 10.0f;
	other.playing = true;
	reg->emplace<scn::animated_node_component>(other_bone, scn::animated_node_component{ "Bone" });
	emit_hierarchy(*reg, root);
	emit_hierarchy(*reg, other_root);
	emit_transform(*reg, other_root);

	run_for(0.5f); // 5 ticks -> halfway along the 0..10 position track
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(bone).local[3]) == glm::vec3{ 5, 0, 0 }));
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(twin).local[3]) == glm::vec3{ 5, 0, 0 }));
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(other_bone).local[3]) == glm::vec3{ 0, 0, 0 }));
}

struct anim_despawn_fixture : anim_fixture
{
	anim_despawn_fixture() : anim_fixture(true) {}
};

// The demo crash (#543): the animation queues the bone's transform_updated in `update`, game code destroys the
// bone later in the same phase, and phase::transform then consumes the queue and walks the parent's children.
BOOST_FIXTURE_TEST_CASE(Anim_GameDespawnsAnimatedNodeInTheFrameItWasAnimated, anim_despawn_fixture)
{
	run_for(0.25f);
	reg->ctx().emplace<despawn_request>().root = bone;
	emit_transform(*reg, root);
	step(0.1f);

	BOOST_TEST(!reg->valid(bone));
	BOOST_TEST(reg->get<scn::children_component>(root).children.empty());

	reg->get<scn::local_transform>(root).local = glm::translate(glm::mat4(1.0f), glm::vec3{ 3, 0, 0 });
	emit_transform(*reg, root);
	step(0.1f);
	BOOST_TEST((world_pos(*reg, root) == glm::vec3{ 3, 0, 0 }));
}

// Only the queue path: nothing reports the parent in that frame, so its list is not walked.
BOOST_FIXTURE_TEST_CASE(Anim_GameDespawnsAnimatedNodeInTheFrameItWasAnimated_QueueOnly, anim_despawn_fixture)
{
	run_for(0.25f);
	reg->ctx().emplace<despawn_request>().root = bone;
	step(0.1f);

	BOOST_TEST(!reg->valid(bone));
	BOOST_TEST(reg->get<scn::children_component>(root).children.size() == 1u); // still listed: no walk this frame
	BOOST_TEST(reg->ctx().get<ecs::event<scn::transform_updated>>().size() == 0u);
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

// The camera runs before the transform phase, so this frame's move is already in the world matrix.
struct camera_world_fixture : systems_fixture
{
	camera_world_fixture()
	{
		auto& world = make_world({ "scn::depth_system", "scn::transform_system", "scn::update_camera_matrix_system" });
		reg = &world.state();
		state = &reg->ctx().emplace<inp::input_state>();
		reg->ctx().emplace<inp::input_focus>();
		cam = node(*reg, entt::null, { 0, 0, 0 });
		reg->emplace<scn::mouse_controller_component>(cam);
	}

	entt::registry* reg = nullptr;
	inp::input_state* state = nullptr;
	entt::entity cam = entt::null;
};

BOOST_FIXTURE_TEST_CASE(Camera_WorldMatrixReflectsThisFramesMove, camera_world_fixture)
{
	emit_hierarchy(*reg, cam);
	step(0.016f);

	state->mouse[(int)inp::MOUSE_BUTTONS::LEFT] = true;
	state->mouse_dir = { 0.1f, 0.2f };
	step(0.016f);

	const glm::vec3 local = glm::vec3(reg->get<scn::local_transform>(cam).local[3]);
	BOOST_TEST(local.z == 0.6f + 2.0f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST((world_pos(*reg, cam) == local)); // not the previous frame's matrix
}

BOOST_FIXTURE_TEST_CASE(Camera_ScrollUpZoomsInAndClampsAtZero, camera_fixture)
{
	state->scroll = { 0.0f, 1.0f };
	step(0.016f);
	BOOST_TEST(ctl().distance == 0.0f); // 2 - 1*3 clamped to 0
	BOOST_TEST(events() == 1u);
}

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

// Bug #545 (F11): a game camera carried a mouse controller, so every drag that reaches the world moved it too.
BOOST_FIXTURE_TEST_CASE(Camera_ABuiltGameCameraIsNotMovedByTheMouse, camera_fixture)
{
	const auto game = reg->create();
	reg->emplace<scn::local_transform>(game, glm::translate(glm::mat4(1.0f), glm::vec3(0.f, 5.f, 0.f)));
	reg->emplace<scn::world_transform>(game);
	reg->emplace<scn::depth_level>(game);
	scn::assemble_camera(*reg, game, scn::camera_desc{}, "game");

	state->mouse[(int)inp::MOUSE_BUTTONS::LEFT] = true;
	state->mouse_dir = { 0.1f, 0.2f };
	step(0.016f);

	BOOST_TEST(ctl().position.x == 0.3f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(!reg->all_of<scn::mouse_controller_component>(game));
	BOOST_TEST((glm::vec3(reg->get<scn::local_transform>(game).local[3]) == glm::vec3(0.f, 5.f, 0.f)));
}

BOOST_AUTO_TEST_SUITE_END()
