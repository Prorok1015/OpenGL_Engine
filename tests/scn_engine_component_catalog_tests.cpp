#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_camera_controller_component.hpp>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/scene/level/scn_screen_anchor.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <type_traits>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Every runtime component the engine creates is in the base catalog with a schema, or marked derived / editor-only
// (#599, HOT_RELOAD_PLAY.md section 4.3). A world is spawned through the production spawners with a node per scene desc
// (k_every_desc), and every non-empty storage must be known: a component one of these spawners adds without registering
// it makes the walk report it, which fails the test. EverySpawnableDescIsInThePrefab keeps that prefab complete: a desc
// type with a spawner that is neither in it nor in k_not_spawned fails (#620). Components made outside the spawners are
// checked in the sources by tools/ci/check_engine_components.py.

namespace
{
	// A prefab with one node per scene desc; a skinning node with bones adds the skeleton and the bone matrices to the
	// root.
	constexpr const char* k_every_desc = R"json({"__type":"prefab_desc","__version":4,"children":[
				{"__id":"00000000000000a1","name":"Cam","components":{"camera":{"__type":"camera_desc","__version":2}}},
				{"__id":"00000000000000a2","name":"Sun","components":{"light":{"__type":"directional_light_desc"}}},
				{"__id":"00000000000000a3","name":"Marker","components":{"marker":{"__type":"screen_anchor_desc"}}},
				{"__id":"00000000000000a4","name":"Sky","components":{"sky":{"__type":"skybox_desc"}}},
				{"__id":"00000000000000a5","name":"Mesh","components":{"mesh":{"__type":"mesh_node_desc"},
					"skin":{"__type":"skinning_desc","bone_count":2}}},
				{"__id":"00000000000000a6","name":"Object","components":{"object":{"__type":"object_desc"}}},
				{"__id":"00000000000000a7","name":"Bone","components":{"bone":{"__type":"bone_desc"}}},
				{"__id":"00000000000000a8","name":"Animator","components":{"controller":{"__type":"animation_controller_desc"},
					"clips":{"__type":"animation_collection_desc"}}},
				{"__id":"00000000000000a9","name":"Animated","components":{"node":{"__type":"animated_node_desc"}}},
				{"__id":"00000000000000aa","name":"Anchor","components":{"anchor":{"__type":"anchor_desc"}},
					"children":[{"__id":"00000000000000ab","name":"Child","components":{"object":{"__type":"object_desc"}}}]}
			]})json";

	// Desc types with a spawner that k_every_desc does not spawn, and why. Anything else with a spawner must be in it.
	struct not_spawned
	{
		std::string_view type;
		std::string_view why;
	};
	constexpr not_spawned k_not_spawned[] = {
		{ "prefab_desc", "structural: the prefab itself, whose nodes are the other descs" },
		{ "world_desc", "structural: a world is a prefab, spawned like prefab_desc" },
		{ "skin_weights_desc", "creates no component: it registers the weights in rnd::skinning_manager" },
		{ "stat_desc", "test-only desc of test_scene_env" },
	};

	// The __type of every object in `value`, at any depth.
	void collect_types(const boost::json::value& value, std::vector<std::string>& out)
	{
		if (const auto* object = value.if_object()) {
			if (const auto* type = object->if_contains("__type"); type && type->is_string()) out.emplace_back(type->as_string());
			for (const auto& member : *object) collect_types(member.value(), out);
		} else if (const auto* array = value.if_array()) {
			for (const auto& item : *array) collect_types(item, out);
		}
	}

	struct catalog_fixture : test_util::scene_env
	{
		catalog_fixture() { scn::register_engine_components(catalog); }

		entt::entity spawn_every_desc(entt::registry& reg)
		{
			const auto tag = put("all.prefab.desc", k_every_desc);
			const entt::entity root = reg.create();
			scn::assemble_prefab(*assembler, reg, root, *load<scn::prefab_desc>(tag), "all", tag);
			return root;
		}

		scn::component_catalog catalog;
	};

	struct unregistered_component
	{
		int value = 0;
	};

	bool has_storage(const entt::registry& reg, entt::id_type id)
	{
		const auto* storage = reg.storage(id);
		return storage && !storage->empty();
	}
}

BOOST_FIXTURE_TEST_SUITE(EngineComponentCatalog, catalog_fixture)

BOOST_AUTO_TEST_CASE(EveryComponentOfASpawnedWorldIsInTheCatalog)
{
	entt::registry reg;
	spawn_every_desc(reg);

	for (const auto& missing : scn::unregistered_storages(reg, catalog)) {
		BOOST_ERROR("a spawned world holds '" << missing.type << "', which is not in the engine's component catalog");
	}
	// The walk saw the components the prefab is meant to produce.
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::camera_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::bone_matrices_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::parent_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::children_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::scene_anchor_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::renderable>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::directional_light>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::screen_anchor_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::skeleton_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::animated_node_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::mesh_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::animation_controller_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::object_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::sky_component>::value()));
	BOOST_TEST(has_storage(reg, entt::type_hash<scn::bone_component>::value()));
}

// A new desc type with a spawner is checked by EveryComponentOfASpawnedWorldIsInTheCatalog only once it is in the
// prefab: until then this fails, naming it.
BOOST_AUTO_TEST_CASE(EverySpawnableDescIsInThePrefab)
{
	std::vector<std::string> spawned;
	collect_types(boost::json::parse(k_every_desc), spawned);
	std::size_t checked = 0;
	for (const std::string& type : assembler->spawner_types()) {
		++checked;
		const bool in_prefab = std::ranges::find(spawned, type) != spawned.end();
		const bool excused = std::ranges::any_of(k_not_spawned, [&](const not_spawned& entry) { return entry.type == type; });
		BOOST_CHECK_MESSAGE(in_prefab || excused, "desc type '" << type << "' has a spawner but is not in k_every_desc: add a node "
			"with it there (its components are then checked), or to k_not_spawned with the reason");
	}
	BOOST_TEST(checked > std::size(k_not_spawned));
	// An excuse for a type that has no spawner any more is stale.
	for (const not_spawned& entry : k_not_spawned) {
		BOOST_CHECK_MESSAGE(assembler->has_spawner(entry.type), "k_not_spawned names '" << entry.type << "', which has no spawner: remove it");
	}
}

BOOST_AUTO_TEST_CASE(EveryEngineEntryHasASchemaOrIsMarked)
{
	catalog.for_each([](const scn::component_entry& entry) {
		const bool marked = entry.category == scn::component_category::derived || entry.category == scn::component_category::editor_only;
		BOOST_TEST((entry.schema || marked), entry.name << " has no schema and is not marked derived or editor-only");
	});
}

BOOST_AUTO_TEST_CASE(AComponentOutsideTheCatalogIsReported)
{
	entt::registry reg;
	const entt::entity root = spawn_every_desc(reg);
	reg.emplace<unregistered_component>(root);

	const auto missing = scn::unregistered_storages(reg, catalog);
	BOOST_REQUIRE_EQUAL(missing.size(), 1u);
	BOOST_TEST(missing.front().id == entt::type_hash<unregistered_component>::value());
	BOOST_TEST(missing.front().type.find("unregistered_component") != std::string::npos);

	// A storage whose components are all gone is not a component of the world any more.
	reg.remove<unregistered_component>(root);
	BOOST_TEST(scn::unregistered_storages(reg, catalog).empty());
}

BOOST_AUTO_TEST_CASE(DescribingAComponentKeepsItsLayout)
{
	// The schemas are described from outside: the components keep their size (no member, base or vtable added).
	BOOST_TEST(sizeof(scn::local_transform) == sizeof(glm::mat4));
	BOOST_TEST(sizeof(scn::parent_component) == sizeof(entt::entity));
	BOOST_TEST(std::is_empty_v<scn::renderable>);
	BOOST_TEST(!std::is_polymorphic_v<scn::name_component>);
	BOOST_TEST(std::is_aggregate_v<scn::mouse_controller_component>);
}

BOOST_AUTO_TEST_CASE(ALayerSeesTheEngineComponents)
{
	scn::component_catalog layer{ &catalog };
	BOOST_REQUIRE(layer.find("scn.local_transform"));
	BOOST_TEST(layer.find(entt::type_hash<scn::local_transform>::value())->saved());
	BOOST_TEST(!layer.add<unregistered_component>("scn.local_transform", nullptr));
}

BOOST_AUTO_TEST_CASE(Batch2ComponentsRoundTripWithoutResources)
{
	entt::registry source;
	const entt::entity skeleton = source.create();
	const entt::entity mesh = source.create();
	scn::camera_component camera;
	camera.fov = 60.f;
	camera.layer = 2;
	camera.clear = scn::camera_clear::depth;
	camera.projection = scn::camera_projection::ortho;
	camera.color_targets.push_back(res::tag{ "memory://rt/main" });
	source.emplace<scn::camera_component>(mesh, camera);
	source.emplace<scn::mesh_component>(mesh, scn::mesh_component{ scn::mesh_view{ 3, 9, 12, 30 } });
	source.emplace<scn::skinning_component>(mesh, scn::skinning_component{ res::tag{ "memory://skin" }, skeleton });
	source.emplace<scn::animation_controller_component>(mesh, scn::animation_controller_component{ "run", 1.25f, 2.f, true, false, 4.f, 30.f });
	scn::component_tracker tracker;
	tracker.tracked_components["camera"] = scn::tracked_component_info{ "camera_desc", res::tag{ "memory://cam.desc" }, json::object{ { "fov", 60 } } };
	source.emplace<scn::component_tracker>(mesh, tracker);

	entt::registry target;
	(void)target.create();
	const entt::entity skeleton_restored = target.create();
	const entt::entity mesh_restored = target.create();
	struct remap final : desc::entity_remap
	{
		std::uint32_t from = 0, to = 0;
		std::optional<std::uint32_t> find(std::uint32_t saved) const override { return saved == from ? std::optional{ to } : std::nullopt; }
	} ids;
	ids.from = static_cast<std::uint32_t>(entt::to_integral(skeleton));
	ids.to = static_cast<std::uint32_t>(entt::to_integral(skeleton_restored));
	desc::read_context ctx;
	ctx.entities = &ids;

	for (std::string_view name : { "scn.camera", "scn.mesh", "scn.skinning", "scn.animation_controller", "scn.component_tracker" }) {
		const scn::component_entry* entry = catalog.find(name);
		BOOST_REQUIRE(entry && entry->schema);
		json::object data;
		BOOST_REQUIRE(entry->write(*entry->schema, source, mesh, data));
		entry->read(*entry->schema, target, mesh_restored, data, ctx);
	}

	const auto& cam = target.get<scn::camera_component>(mesh_restored);
	BOOST_TEST(cam.fov == 60.f);
	BOOST_TEST(cam.layer == 2);
	BOOST_TEST((cam.clear == scn::camera_clear::depth));
	BOOST_TEST((cam.projection == scn::camera_projection::ortho));
	BOOST_REQUIRE_EQUAL(cam.color_targets.size(), 1u);
	BOOST_TEST(cam.color_targets[0].view() == "memory://rt/main");
	const auto& view = target.get<scn::mesh_component>(mesh_restored).mesh;
	BOOST_TEST(view.vx_begin == 3u);
	BOOST_TEST(view.vx_end == 9u);
	BOOST_TEST(view.ind_begin == 12u);
	BOOST_TEST(view.ind_end == 30u);
	const auto& skin = target.get<scn::skinning_component>(mesh_restored);
	BOOST_TEST(skin.skinning_tag.view() == "memory://skin");
	BOOST_TEST(entt::to_integral(skin.skeleton_entity) == entt::to_integral(skeleton_restored));
	const auto& anim = target.get<scn::animation_controller_component>(mesh_restored);
	BOOST_TEST(anim.current_animation == "run");
	BOOST_TEST(anim.current_time == 1.25f);
	BOOST_TEST(anim.speed == 2.f);
	BOOST_TEST(anim.playing);
	BOOST_TEST(!anim.loop);
	BOOST_TEST(anim.duration == 4.f);
	BOOST_TEST(anim.ticks_per_second == 30.f);
	const auto& tracked = target.get<scn::component_tracker>(mesh_restored).tracked_components;
	BOOST_REQUIRE(tracked.contains("camera"));
	BOOST_TEST(tracked.at("camera").type_name == "camera_desc");
	BOOST_TEST(tracked.at("camera").source_desc.view() == "memory://cam.desc");
	BOOST_TEST(tracked.at("camera").local_overrides.at("fov").to_number<int>() == 60);
}

BOOST_AUTO_TEST_CASE(Batch2ResourceFreeComponentsRoundTripFieldByField)
{
	entt::registry source;
	const entt::entity node = source.create();
	glm::mat4 offset{ 1.0f };
	offset[3] = glm::vec4{ 1.5f, -2.f, 0.25f, 1.f };
	source.emplace<scn::geometry_component>(node, scn::geometry_component{ res::tag{ "memory://mesh.geom" } });
	source.emplace<scn::skeleton_component>(node, scn::skeleton_component{ res::tag{ "memory://skin" }, 12, true });
	source.emplace<scn::sky_component>(node, scn::sky_component{ { res::tag{ "memory://px" }, res::tag{ "memory://nx" } } });
	source.emplace<scn::bone_component>(node, scn::bone_component{ offset, 7 });
	source.emplace<scn::animated_node_component>(node, scn::animated_node_component{ "Spine" });
	source.emplace<scn::skinning_component>(node, scn::skinning_component{ res::tag{ "memory://skin" }, entt::null });
	scn::camera_component camera;
	camera.m_viewport = scn::viewport{ glm::ivec4{ 10, 20, 640, 480 } };
	source.emplace<scn::camera_component>(node, camera);
	scn::component_tracker tracker;
	tracker.tracked_components["zeta"] = scn::tracked_component_info{ "b_desc", res::tag{ "memory://b.desc" }, {} };
	tracker.tracked_components["alpha"] = scn::tracked_component_info{ "a_desc", res::tag{ "memory://a.desc" }, {} };
	source.emplace<scn::component_tracker>(node, tracker);

	struct no_ids final : desc::entity_remap
	{
		std::optional<std::uint32_t> find(std::uint32_t) const override { return std::nullopt; }
	} ids;
	desc::read_context ctx;
	ctx.entities = &ids;
	entt::registry target;
	const entt::entity restored = target.create();
	for (std::string_view name : { "scn.geometry", "scn.skeleton", "scn.sky", "scn.bone", "scn.animated_node", "scn.skinning", "scn.camera", "scn.component_tracker" }) {
		const scn::component_entry* entry = catalog.find(name);
		BOOST_REQUIRE(entry && entry->schema);
		json::object data;
		BOOST_REQUIRE(entry->write(*entry->schema, source, node, data));
		if (name == "scn.component_tracker") {
			const auto& tracked = data.at("tracked_components").as_object();
			BOOST_REQUIRE_EQUAL(tracked.size(), 2u);
			BOOST_TEST(tracked.begin()->key() == "alpha"); // written in name order
		}
		BOOST_TEST(entry->read(*entry->schema, target, restored, data, ctx), name);
	}

	BOOST_TEST(target.get<scn::geometry_component>(restored).geom_tag.view() == "memory://mesh.geom");
	const auto& skeleton = target.get<scn::skeleton_component>(restored);
	BOOST_TEST(skeleton.skinning_tag.view() == "memory://skin");
	BOOST_TEST(skeleton.bone_count == 12);
	BOOST_TEST(skeleton.show_skeleton);
	const auto& sky = target.get<scn::sky_component>(restored).cube_map;
	BOOST_REQUIRE_EQUAL(sky.size(), 2u);
	BOOST_TEST(sky[1].view() == "memory://nx");
	const auto& bone = target.get<scn::bone_component>(restored);
	BOOST_TEST(std::memcmp(&bone.offset, &offset, sizeof(glm::mat4)) == 0);
	BOOST_TEST(bone.index == 7);
	BOOST_TEST(target.get<scn::animated_node_component>(restored).node_name == "Spine");
	BOOST_TEST((target.get<scn::skinning_component>(restored).skeleton_entity == entt::null));
	const glm::ivec4 viewport = target.get<scn::camera_component>(restored).m_viewport;
	BOOST_TEST((viewport == glm::ivec4{ 10, 20, 640, 480 }));
	const auto& tracked = target.get<scn::component_tracker>(restored).tracked_components;
	BOOST_TEST(tracked.at("zeta").type_name == "b_desc");
	BOOST_TEST(tracked.at("alpha").source_desc.view() == "memory://a.desc");
}

BOOST_AUTO_TEST_SUITE_END()
