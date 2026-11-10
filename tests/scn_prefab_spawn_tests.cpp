#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/common/eng_transform_3d.hpp>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_prefab_spawn.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_transform_system.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

// #551: a prefab spawned and removed by game code with one call each, through the real context_set frame and the real
// depth/transform systems.

namespace {

// What game code asks for during update.
struct spawn_request
{
	res::tag prefab;
	bool despawn_same_frame = false;
	entt::entity spawned = entt::null;
};

struct despawn_request
{
	entt::entity target = entt::null;
};

void spawner_system(entt::registry& reg)
{
	auto* request = reg.ctx().find<spawn_request>();
	if (!request || !request->prefab.is_valid()) return;
	request->spawned = scn::spawn_prefab(reg, request->prefab);
	if (request->despawn_same_frame) scn::despawn(reg, request->spawned);
	request->prefab = res::tag{};
}

void despawner_system(entt::registry& reg)
{
	auto* request = reg.ctx().find<despawn_request>();
	if (!request || request->target == entt::null) return;
	scn::despawn(reg, request->target);
	request->target = entt::null;
}

// The level's own node has the id the prefab's head has: their paths must still differ.
constexpr const char* LEVEL = R"({"__type":"level_desc","__version":2,"name":"lv","worlds":[
	{"__type":"world_desc","__version":4,"name":"w",
	 "systems":["test::spawner","test::despawner","scn::depth_system","scn::transform_system"],
	 "children":[{"__id":"00000000000000a1","name":"level_node"}]}]})";
constexpr const char* SEGMENT = R"({"__type":"prefab_desc","__version":4,"transform":{"position":[0,0,7]},"children":[
	{"__id":"00000000000000a1","name":"head","transform":{"position":[0,1,0]},
	 "children":[{"__id":"00000000000000a2","name":"tail"}]}]})";

struct spawn_fixture : test_util::scene_env
{
	spawn_fixture() : lvl("level", nullptr)
	{
		scn::init_transform_system(sf);
		sf.register_system<&spawner_system>("test::spawner", { .phase = ecs::phase::update, .run_in_edit = true });
		sf.register_system<&despawner_system>("test::despawner",
			{ .phase = ecs::phase::update, .after = { "test::spawner" }, .run_in_edit = true });
		segment = put("segment.prefab.desc", SEGMENT);
		level = load<scn::level_desc>(put("lv.desc", LEVEL));
		lvl.load_from_desc(*level, sf, *assembler);
		reg = &lvl.get("w").state();
	}

	void step() { lvl.update(std::chrono::duration<float>(0.016f)); }

	entt::entity world_root() const
	{
		const auto roots = reg->view<scn::world_root_component>();
		BOOST_REQUIRE_EQUAL(roots.size(), 1u);
		return roots.front();
	}

	glm::vec3 position_of(entt::entity e) const { return glm::vec3(reg->get<scn::local_transform>(e).local[3]); }

	static eng::transform3d placed_at(glm::vec3 position)
	{
		eng::transform3d transform;
		transform.set_pos(position);
		return transform;
	}

	entt::entity at(std::vector<std::uint64_t> path) const { return scn::node_index::get(*reg).find(path); }

	const std::vector<entt::entity>& children_of(entt::entity e) const
	{
		static const std::vector<entt::entity> none;
		const auto* kids = reg->try_get<scn::children_component>(e);
		return kids ? kids->children : none;
	}

	bool lists(entt::entity parent, entt::entity child) const
	{
		return std::ranges::find(children_of(parent), child) != children_of(parent).end();
	}

	std::uint64_t own_id(entt::entity root) const { return reg->get<scn::node_id_component>(root).path.back(); }

	ecs::system_factory sf;
	scn::context_set lvl;
	res::tag segment;
	std::shared_ptr<scn::level_desc> level;
	entt::registry* reg = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnPrefabSpawn, spawn_fixture)

BOOST_AUTO_TEST_CASE(Spawn_BuildsThePrefabUnderTheWorldRoot_WithNodePathsOfItsOwn)
{
	const entt::entity root = scn::spawn_prefab(*reg, segment, placed_at({ 5.0f, 0.0f, 0.0f }));
	BOOST_REQUIRE(root != entt::null);
	BOOST_TEST(reg->all_of<scn::spawned_root_component>(root));
	BOOST_TEST((reg->get<scn::parent_component>(root).parent == world_root()));
	BOOST_TEST(lists(world_root(), root));
	BOOST_REQUIRE_EQUAL(reg->get<scn::node_id_component>(root).path.size(), 1u);
	BOOST_TEST(own_id(root) != 0xa1u);
	BOOST_TEST((reg->get<scn::prefab_instance>(root).source_prefab == segment));
	BOOST_TEST(reg->get<scn::name_component>(root).name == "segment");

	const entt::entity level_node = at({ 0xa1 });
	BOOST_REQUIRE(level_node != entt::null);
	BOOST_TEST(reg->get<scn::name_component>(level_node).name == "level_node");
	const entt::entity head = at({ own_id(root), 0xa1 });
	const entt::entity tail = at({ own_id(root), 0xa1, 0xa2 });
	BOOST_REQUIRE(head != entt::null);
	BOOST_REQUIRE(tail != entt::null);
	BOOST_TEST(reg->get<scn::name_component>(head).name == "head");
	BOOST_TEST((reg->get<scn::parent_component>(head).parent == root));

	step();
	const auto depth = [this](entt::entity e) { return reg->get<scn::depth_level>(e).value; };
	BOOST_TEST(depth(root) == depth(world_root()) + 1);
	BOOST_TEST(depth(head) == depth(root) + 1);
	BOOST_TEST(depth(tail) == depth(head) + 1);
	const glm::vec4 head_at = reg->get<scn::world_transform>(head).world[3];
	BOOST_TEST(head_at.x == 5.0f);
	BOOST_TEST(head_at.y == 1.0f);
	BOOST_TEST(head_at.z == 0.0f);
}

BOOST_AUTO_TEST_CASE(Spawn_WithoutATransform_KeepsThePrefabRootsOwn_WithOne_SetsExactlyIt)
{
	const entt::entity own = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(own != entt::null);
	BOOST_TEST((position_of(own) == glm::vec3(0.0f, 0.0f, 7.0f)));

	const entt::entity identity = scn::spawn_prefab(*reg, segment, eng::transform3d{});
	BOOST_REQUIRE(identity != entt::null);
	BOOST_TEST((reg->get<scn::local_transform>(identity).local == glm::mat4(1.0f)));

	step();
	BOOST_TEST((glm::vec3(reg->get<scn::world_transform>(own).world[3]) == glm::vec3(0.0f, 0.0f, 7.0f)));
	BOOST_TEST((glm::vec3(reg->get<scn::world_transform>(identity).world[3]) == glm::vec3(0.0f)));
}

BOOST_AUTO_TEST_CASE(Spawn_UnderAParent_ExtendsItsPath)
{
	const entt::entity first = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(first != entt::null);
	const entt::entity head = at({ own_id(first), 0xa1 });
	BOOST_REQUIRE(head != entt::null);

	const entt::entity second = scn::spawn_prefab(*reg, segment, std::nullopt, head);
	BOOST_REQUIRE(second != entt::null);
	BOOST_TEST((reg->get<scn::parent_component>(second).parent == head));
	BOOST_TEST(lists(head, second));
	BOOST_TEST((at({ own_id(first), 0xa1, own_id(second), 0xa1, 0xa2 }) != entt::null));
}

BOOST_AUTO_TEST_CASE(Spawn_IsRefused_InAWorldNotBuiltFromADesc_UnderAParentThatIsGone_OrForAPrefabThatDoesNotLoad)
{
	auto& bare = lvl.create_context("bare", lvl.acquire_free_id());
	BOOST_TEST((scn::spawn_prefab(bare.state(), segment) == entt::null));

	const entt::entity gone = reg->create();
	reg->destroy(gone);
	BOOST_TEST((scn::spawn_prefab(*reg, segment, std::nullopt, gone) == entt::null));

	BOOST_TEST((scn::spawn_prefab(*reg, tag_of("missing.prefab.desc")) == entt::null));
	BOOST_TEST(children_of(world_root()).empty());
}

BOOST_AUTO_TEST_CASE(Spawn_ByATagNotLoadedYet_LoadsItNow_AndSaysSoOnce)
{
	std::size_t waits = 0;
	const engine::log_sink_handle sink = engine::add_log_sink([&waits](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::warn && message.find("not loaded yet") != std::string_view::npos) ++waits;
	});
	const res::tag late = put("late.prefab.desc", SEGMENT);
	// No pump between: only the synchronous load can have built them.
	const entt::entity first = scn::spawn_prefab(*reg, late);
	const entt::entity second = scn::spawn_prefab(*reg, late);
	BOOST_TEST((first != entt::null));
	BOOST_TEST((second != entt::null));
	BOOST_TEST((at({ own_id(first), 0xa1, 0xa2 }) != entt::null));
	BOOST_TEST(waits == 1u);
}

BOOST_AUTO_TEST_CASE(AHotReloadOfThePrefabSpawnedUnder_KeepsTheSpawnedTree)
{
	const entt::entity first = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(first != entt::null);
	const entt::entity head = at({ own_id(first), 0xa1 });
	BOOST_REQUIRE(head != entt::null);
	const entt::entity second = scn::spawn_prefab(*reg, segment, std::nullopt, head);
	BOOST_REQUIRE(second != entt::null);
	const entt::entity second_tail = at({ own_id(first), 0xa1, own_id(second), 0xa1, 0xa2 });
	BOOST_REQUIRE(second_tail != entt::null);

	put("segment.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
		{"__id":"00000000000000a1","name":"head","transform":{"position":[0,2,0]},
		 "children":[{"__id":"00000000000000a2","name":"tail"}]}]})");
	rs->signal_changed(segment);
	pump();
	scn::trigger_hot_reload(*assembler, *reg, require_loaded<desc::desc_base>(segment));

	BOOST_TEST(reg->valid(head));
	BOOST_TEST((position_of(head) == glm::vec3(0.0f, 2.0f, 0.0f)));
	BOOST_TEST(reg->valid(second));
	BOOST_TEST(lists(head, second));
	BOOST_TEST(reg->valid(second_tail));
	step();
}

BOOST_AUTO_TEST_CASE(DespawnOfAMiddleNode_DuringUpdate_LeavesTheDepthAndTransformSystemsWhole)
{
	const entt::entity root = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(root != entt::null);
	const std::vector<std::uint64_t> head_path{ own_id(root), 0xa1 };
	const entt::entity head = at(head_path);
	const entt::entity tail = at({ own_id(root), 0xa1, 0xa2 });
	BOOST_REQUIRE(head != entt::null);
	// The spawn left transform and hierarchy events of the head and the tail queued for this frame's transform phase.
	reg->ctx().emplace<despawn_request>(despawn_request{ head });

	step();
	BOOST_TEST(!reg->valid(head));
	BOOST_TEST(!reg->valid(tail));
	BOOST_TEST(reg->valid(root));
	BOOST_TEST(!lists(root, head));
	BOOST_TEST((at(head_path) == entt::null));
	step();
	BOOST_TEST(reg->valid(root));
}

BOOST_AUTO_TEST_CASE(DespawnOfATreeSpawnedInTheSameFrame_RemovesItWhole)
{
	reg->ctx().emplace<spawn_request>(spawn_request{ segment, true, entt::null });
	step();
	const entt::entity spawned = reg->ctx().get<spawn_request>().spawned;
	BOOST_REQUIRE(spawned != entt::null);
	BOOST_TEST(!reg->valid(spawned));
	BOOST_TEST(!lists(world_root(), spawned));
	BOOST_TEST(reg->valid(at({ 0xa1 })));
	step();
}

BOOST_AUTO_TEST_CASE(SpawnUnderAParentQueuedForDespawn_GoesWithIt)
{
	const entt::entity first = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(first != entt::null);
	const entt::entity head = at({ own_id(first), 0xa1 });
	scn::despawn(*reg, first);
	const entt::entity second = scn::spawn_prefab(*reg, segment, std::nullopt, head);
	BOOST_REQUIRE(second != entt::null);

	step();
	BOOST_TEST(!reg->valid(first));
	BOOST_TEST(!reg->valid(second));
	BOOST_TEST(!lists(world_root(), first));
	step();
}

BOOST_AUTO_TEST_CASE(DespawnTwice_IsOnce_AndTheWorldRootIsRefused)
{
	const entt::entity root = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(root != entt::null);
	scn::despawn(*reg, root);
	scn::despawn(*reg, root);
	step();
	BOOST_TEST(!reg->valid(root));
	scn::despawn(*reg, root); // gone: nothing
	scn::despawn(*reg, world_root());
	step();
	BOOST_TEST(reg->valid(world_root()));
	BOOST_TEST(reg->valid(at({ 0xa1 })));
}

BOOST_AUTO_TEST_CASE(AWorldBuiltAgain_SpawnsUnderItsNewRoot)
{
	const auto world = level->get_worlds().front().get_sync();
	BOOST_REQUIRE(world);
	lvl.reload_context("w", *world, sf, *assembler);
	reg = &lvl.get("w").state();
	const entt::entity spawned = scn::spawn_prefab(*reg, segment);
	BOOST_REQUIRE(spawned != entt::null);
	BOOST_TEST((reg->get<scn::parent_component>(spawned).parent == world_root()));
}

BOOST_AUTO_TEST_CASE(BindPrefabSpawning_FindsTheRootOfARestoredWorld)
{
	// What a snapshot restores: the root keeps its marker, nothing is built from the desc.
	entt::registry restored;
	const entt::entity root = restored.create();
	restored.emplace<scn::node_id_component>(root);
	BOOST_TEST(!scn::bind_prefab_spawning(restored, *assembler));
	BOOST_TEST((scn::spawn_prefab(restored, segment) == entt::null));

	restored.emplace<scn::world_root_component>(root);
	BOOST_REQUIRE(scn::bind_prefab_spawning(restored, *assembler));
	const entt::entity spawned = scn::spawn_prefab(restored, segment);
	BOOST_REQUIRE(spawned != entt::null);
	BOOST_TEST((restored.get<scn::parent_component>(spawned).parent == root));
}

BOOST_AUTO_TEST_SUITE_END()
