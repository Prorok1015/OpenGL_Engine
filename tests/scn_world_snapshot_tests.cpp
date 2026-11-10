#include <boost/test/unit_test.hpp>
#include <boost/json/serialize.hpp>
#include "test_scene_env.hpp"
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/scn_blackboard_catalog.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_component_schema.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_skybox_desc.h>
#include <snk/scene/scn_transform_system.h>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_system.h>
#include <memory>
#include <snk/ecs/ecs_random.hpp>
#include <entt/entt.hpp>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Snapshot and restore of a live world (#599, HOT_RELOAD_PLAY.md sections 4 and 6): every saved component comes back
// field by field, entity references point at the restored entities, runtime entities without a node path come back
// too, time and the random stream continue where they stood, and what cannot be saved is reported.

namespace snapshot_test
{
	// A module's runtime entity: no node path, a reference to another entity, an id the test finds it by.
	struct bullet
	{
		int id = 0;
		entt::entity owner = entt::null;
		float speed = 0.f;
	};
	BOOST_DESCRIBE_STRUCT(bullet, (), (id, owner, speed))

	struct score
	{
		int points = 0;
	};
	BOOST_DESCRIBE_STRUCT(score, (), (points))

	struct scratch
	{
		int value = 0;
	};

	struct no_schema
	{
		int value = 0;
	};

	struct editor_marker
	{
	};

	template<typename T>
	std::shared_ptr<const desc::schema_binding> schema_of(std::string_view name)
	{
		return desc::bind_described<T>(name, 1, desc::schema_kind::component);
	}

	// Components whose fields hold entities: compared through the mapping, the rest as JSON.
	const std::set<std::string, std::less<>> k_entity_fields = { "scn.parent", "scn.children", "scn.skinning", "test.bullet" };

	struct snapshot_fixture : test_util::scene_env
	{
		snapshot_fixture()
			: layer{ &catalog }
		{
			scn::register_engine_components(catalog);
			layer.add<bullet>("test.bullet", schema_of<bullet>("test.bullet"));
			layer.add_ctx<score>("test.score", schema_of<score>("test.score"));
			layer.add<no_schema>("test.no_schema", nullptr);
			layer.add_editor_only<editor_marker>("test.editor_marker");
		}

		void spawn(entt::registry& reg)
		{
			const auto tag = put("snap.prefab.desc", R"({"__type":"prefab_desc","__version":4,"children":[
				{"__id":"00000000000000b1","name":"Cam","transform":{"position":[0,2,5]},
					"components":{"camera":{"__type":"camera_desc","__version":2,"fov":70}}},
				{"__id":"00000000000000b2","name":"Sun","components":{"light":{"__type":"directional_light_desc"}}},
				{"__id":"00000000000000b3","name":"Body","transform":{"position":[1,0,0]},"components":{
					"mesh":{"__type":"mesh_node_desc","vx_begin":4,"vx_end":10,"ind_begin":0,"ind_end":12},
					"skin":{"__type":"skinning_desc","bone_count":3}},
					"children":[{"__id":"00000000000000b4","name":"Hand","transform":{"rotation":[0,45,0]},
						"components":{"bone":{"__type":"bone_desc"}}}]},
				{"__id":"00000000000000b5","name":"Animator","components":{"controller":{"__type":"animation_controller_desc"}}}
			]})");
			const entt::entity root = reg.create();
			scn::assemble_prefab(*assembler, reg, root, *load<scn::prefab_desc>(tag), "level", tag);
		}

		entt::entity find_named(const entt::registry& reg, std::string_view name) const
		{
			for (const auto [entity, named] : reg.view<scn::name_component>().each()) {
				if (named.name == name) {
					return entity;
				}
			}
			return entt::null;
		}

		// The restored entity of `entity`: by node path, else by the bullet id.
		entt::entity counterpart(const entt::registry& from, entt::entity entity, entt::registry& to) const
		{
			if (entity == entt::null) {
				return entt::null;
			}
			if (const auto* path = from.try_get<scn::node_id_component>(entity); path && !path->path.empty()) {
				return scn::node_index::get(to).find(path->path);
			}
			if (const auto* named = from.try_get<scn::name_component>(entity)) {
				return find_named(to, named->name);
			}
			if (const auto* shot = from.try_get<bullet>(entity)) {
				for (const auto [other, restored] : to.view<bullet>().each()) {
					if (restored.id == shot->id) {
						return other;
					}
				}
			}
			return entt::null;
		}

		json::object value_of(const scn::component_entry& entry, const entt::registry& reg, entt::entity entity) const
		{
			json::object out;
			entry.write(*entry.schema, reg, entity, out);
			return out;
		}

		desc::read_context read_context() const
		{
			desc::read_context ctx;
			ctx.system = descs.get();
			return ctx;
		}

		scn::component_catalog catalog;
		scn::component_catalog layer;
	};
}

using namespace snapshot_test;

BOOST_FIXTURE_TEST_SUITE(WorldSnapshot, snapshot_fixture)

BOOST_AUTO_TEST_CASE(EverySavedComponentComesBackFieldByField)
{
	scn::world_context source{ 1, "main" };
	entt::registry& from = source.state();
	spawn(from);
	const entt::entity body = find_named(from, "Body");
	BOOST_REQUIRE(body != entt::null);
	const entt::entity first = from.create();
	from.emplace<bullet>(first, bullet{ 1, body, 4.5f });
	const entt::entity second = from.create();
	from.emplace<bullet>(second, bullet{ 2, first, 9.f });
	from.ctx().emplace<score>(score{ 17 });

	scn::snapshot_report captured;
	const json::object snapshot = scn::capture_world(source, layer, captured);
	BOOST_TEST(captured.unsaved.empty());

	scn::world_context target{ 1, "main" };
	entt::registry& to = target.state();
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), restored));
	BOOST_TEST(restored.unknown.empty());
	BOOST_TEST(restored.partial.empty());
	BOOST_TEST(restored.entities == captured.entities);

	std::size_t compared = 0;
	for (const entt::entity entity : *std::as_const(from).storage<entt::entity>()) {
		if (!from.valid(entity)) {
			continue;
		}
		const entt::entity copy = counterpart(from, entity, to);
		BOOST_REQUIRE_MESSAGE(copy != entt::null, "an entity was not restored");
		layer.for_each([&](const scn::component_entry& entry) {
			if (entry.category != scn::component_category::data || !entry.saved() || !entry.contains(from, entity)) {
				return;
			}
			BOOST_REQUIRE_MESSAGE(entry.contains(to, copy), entry.name << " was not restored");
			++compared;
			if (k_entity_fields.contains(entry.name)) {
				return;
			}
			// Parenthesised: Boost.Test would compare json::object as a collection, which MSVC cannot instantiate.
			const json::object before = value_of(entry, from, entity);
			const json::object after = value_of(entry, to, copy);
			BOOST_TEST((before == after), entry.name << " differs after the restore: " << json::serialize(before) << " vs " << json::serialize(after));
		});

		if (const auto* parent = from.try_get<scn::parent_component>(entity)) {
			BOOST_TEST((to.get<scn::parent_component>(copy).parent == counterpart(from, parent->parent, to)));
		}
		if (const auto* children = from.try_get<scn::children_component>(entity)) {
			const auto& restored_children = to.get<scn::children_component>(copy).children;
			BOOST_REQUIRE_EQUAL(restored_children.size(), children->children.size());
			for (std::size_t i = 0; i < restored_children.size(); ++i) {
				BOOST_TEST((restored_children[i] == counterpart(from, children->children[i], to)));
			}
		}
		if (const auto* skin = from.try_get<scn::skinning_component>(entity)) {
			BOOST_TEST((to.get<scn::skinning_component>(copy).skeleton_entity == counterpart(from, skin->skeleton_entity, to)));
			BOOST_TEST(to.get<scn::skinning_component>(copy).skinning_tag.view() == skin->skinning_tag.view());
		}
		if (const auto* shot = from.try_get<bullet>(entity)) {
			const auto& copy_shot = to.get<bullet>(copy);
			BOOST_TEST(copy_shot.id == shot->id);
			BOOST_TEST(copy_shot.speed == shot->speed);
			BOOST_TEST((copy_shot.owner == counterpart(from, shot->owner, to)));
		}
	}
	BOOST_TEST(compared > 20u);
	BOOST_TEST(to.ctx().get<score>().points == 17);
	BOOST_TEST(scn::node_index::get(to).size() == scn::node_index::get(from).size());
}

BOOST_AUTO_TEST_CASE(DerivedComponentsAreRebuiltForTheFirstFrame)
{
	scn::world_context source{ 1, "main" };
	spawn(source.state());
	scn::snapshot_report report;
	const json::object snapshot = scn::capture_world(source, layer, report);
	for (const auto& entity : snapshot.at("entities").as_array()) {
		const auto& components = entity.as_object().at("components").as_object();
		BOOST_TEST(!components.contains("scn.world_transform"));
		BOOST_TEST(!components.contains("scn.bone_matrices"));
	}

	scn::world_context target{ 1, "main" };
	entt::registry& to = target.state();
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), report));
	for (const auto entity : to.view<scn::local_transform>()) {
		BOOST_TEST(to.all_of<scn::world_transform>(entity));
	}
	BOOST_TEST(to.ctx().get<ecs::event<scn::transform_updated>>().size() == to.view<scn::local_transform>().size());
	bool skeleton_seen = false;
	for (const auto [entity, skeleton] : to.view<scn::skeleton_component>().each()) {
		skeleton_seen = true;
		BOOST_TEST(to.get<scn::bone_matrices_component>(entity).matrices.size() == static_cast<std::size_t>(skeleton.bone_count));
	}
	BOOST_TEST(skeleton_seen);
}

BOOST_AUTO_TEST_CASE(TimeAndRandomStreamContinueWhereTheyStood)
{
	scn::world_context source{ 3, "main" };
	source.seed(42);
	source.set_mode(ecs::run_mode::play);
	source.begin_frame(std::chrono::duration<float>(0.0371f));
	auto& stream = source.state().ctx().get<ecs::rng>();
	(void)stream.next_u32();
	(void)stream.next_u32();

	scn::snapshot_report report;
	const json::object snapshot = scn::capture_world(source, layer, report);
	scn::world_context target{ 3, "main" };
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), report));

	BOOST_TEST(target.state().ctx().get<scn::world_time>().seconds == source.state().ctx().get<scn::world_time>().seconds);
	BOOST_TEST(target.stepper().accumulator() == source.stepper().accumulator());
	auto& continued = target.state().ctx().get<ecs::rng>();
	BOOST_TEST(continued.seed() == stream.seed());
	for (int i = 0; i < 5; ++i) {
		BOOST_TEST(continued.next_u32() == stream.next_u32());
	}

	target.set_mode(ecs::run_mode::play);
	source.begin_frame(std::chrono::duration<float>(0.02f));
	target.begin_frame(std::chrono::duration<float>(0.02f));
	BOOST_TEST(target.fixed_steps() == source.fixed_steps());
}

BOOST_AUTO_TEST_CASE(RunStateOfTheSetIsRestored)
{
	scn::context_set set("play", nullptr);
	set.step(5);
	set.restore_run_state(1234, 3);
	BOOST_TEST(set.frames_run() == 1234u);
	BOOST_TEST(set.pending_steps() == 3u);

	scn::context_set paused("play", nullptr);
	paused.set_mode(ecs::run_mode::pause);
	paused.restore_run_state(7, 3);
	BOOST_TEST(paused.frames_run() == 7u);
	BOOST_TEST(paused.pending_steps() == 0u);
}

BOOST_AUTO_TEST_CASE(WhatCannotBeSavedOrPlacedIsReported)
{
	scn::world_context source{ 1, "main" };
	entt::registry& from = source.state();
	const entt::entity kept = from.create();
	from.emplace<no_schema>(kept);
	from.emplace<scratch>(kept);
	from.emplace<bullet>(kept, bullet{ 5 });
	const entt::entity editor_camera = from.create();
	from.emplace<editor_marker>(editor_camera);

	scn::snapshot_report captured;
	const json::object snapshot = scn::capture_world(source, layer, captured);
	BOOST_TEST(captured.entities == 1u);
	BOOST_TEST(captured.unsaved.at("test.no_schema") == 1u);
	BOOST_TEST(captured.unsaved.size() == 2u); // and the storage of `scratch`, which the catalog does not know

	scn::world_context target{ 1, "main" };
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, catalog, read_context(), restored)); // the engine's only
	BOOST_TEST(restored.unknown.at("test.bullet") == 1u);
}

// #600 (HOT_RELOAD_PLAY.md 4.4): a blackboard value comes back when its type is registered with a schema; any other is
// reported as lost. Registering is optional and engine types are not affected.
BOOST_AUTO_TEST_CASE(BlackboardValuesWithASchemaComeBack_OthersAreReported)
{
	scn::blackboard_catalog types;
	BOOST_REQUIRE(types.add<score>("test.score", schema_of<score>("test.score")));
	BOOST_TEST(!types.add<scratch>("test.scratch", nullptr)); // only with a schema
	ecs::blackboard_data board;
	board.write("hud.score", entt::type_hash<score>::value(), entt::type_name<score>::value(), std::make_shared<const score>(score{ 17 }), 0);
	board.write("hud.scratch", entt::type_hash<scratch>::value(), entt::type_name<scratch>::value(), std::make_shared<const scratch>(scratch{ 3 }), 0);
	board.commit();

	scn::snapshot_report captured;
	const json::array saved = scn::capture_blackboard(board, types, captured);
	BOOST_REQUIRE(saved.size() == 1u);
	BOOST_TEST(captured.unsaved.at(std::string{ entt::type_name<scratch>::value() }) == 1u);

	ecs::blackboard_data restored_board;
	scn::snapshot_report restored;
	scn::restore_blackboard(restored_board, saved, types, read_context(), restored);
	const ecs::blackboard_reader reader{ &restored_board };
	const score* value = reader.find(ecs::bb_key<score>{ "hud.score" });
	BOOST_REQUIRE(value != nullptr);
	BOOST_TEST(value->points == 17);
	BOOST_TEST(restored_board.committed().size() == 1u);
	BOOST_TEST(restored.unknown.empty());
	BOOST_TEST(restored.partial.empty());

	const scn::blackboard_catalog none;
	ecs::blackboard_data forgotten;
	scn::snapshot_report unknown;
	scn::restore_blackboard(forgotten, saved, none, read_context(), unknown);
	BOOST_TEST(unknown.unknown.at("test.score") == 1u);
	BOOST_TEST(forgotten.committed().empty());

	json::array changed = saved;
	changed.at(0).as_object().at("value").as_object()["points"] = "many";
	ecs::blackboard_data defaulted;
	scn::snapshot_report partial;
	scn::restore_blackboard(defaulted, changed, types, read_context(), partial);
	BOOST_TEST(partial.partial.at("test.score") == 1u);
	const score* kept_default = ecs::blackboard_reader{ &defaulted }.find(ecs::bb_key<score>{ "hud.score" });
	BOOST_REQUIRE(kept_default != nullptr);
	BOOST_TEST(kept_default->points == 0);
}

BOOST_AUTO_TEST_CASE(AFieldThatChangedItsTypeIsReportedAsPartial)
{
	scn::world_context source{ 1, "main" };
	const entt::entity shot = source.state().create();
	source.state().emplace<bullet>(shot, bullet{ 7 });
	scn::snapshot_report report;
	json::object snapshot = scn::capture_world(source, layer, report);
	snapshot.at("entities").as_array().at(0).as_object().at("components").as_object().at("test.bullet").as_object()["speed"] = "fast";

	scn::world_context target{ 1, "main" };
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), restored));
	BOOST_TEST(restored.partial.at("test.bullet") == 1u);
	const auto view = target.state().view<bullet>();
	BOOST_REQUIRE(view.size() == 1u);
	BOOST_TEST(view.get<bullet>(view.front()).id == 7);
	BOOST_TEST(view.get<bullet>(view.front()).speed == 0.f);
}

// #601 (HOT_RELOAD_PLAY.md 7): a field the schema no longer has is dropped, and the report says which type lost one.
BOOST_AUTO_TEST_CASE(AFieldTheSchemaNoLongerHasIsReportedAsRemoved)
{
	scn::world_context source{ 1, "main" };
	source.state().emplace<bullet>(source.state().create(), bullet{ 7 });
	scn::snapshot_report report;
	json::object snapshot = scn::capture_world(source, layer, report);
	snapshot.at("entities").as_array().at(0).as_object().at("components").as_object().at("test.bullet").as_object()["spin"] = 3;

	scn::world_context target{ 1, "main" };
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), restored));
	BOOST_TEST(restored.removed_fields.at("test.bullet") == 1u);
	BOOST_TEST(restored.partial.empty());
	const auto view = target.state().view<bullet>();
	BOOST_REQUIRE(view.size() == 1u);
	BOOST_TEST(view.get<bullet>(view.front()).id == 7);
}

BOOST_AUTO_TEST_CASE(DescReferencesComeBackThroughTheDescSystem)
{
	const auto material_tag = put("plain.material.desc", R"({"__type":"material_desc","queue":"transparent"})");
	const auto sky_tag = put("sky.desc", R"({"__type":"skybox_desc","material":{"__type":"material_desc","queue":"transparent",
		"shader_vertex":"res://shaders/sky.vert"}})");
	const auto clip_tag = put("run.clip.desc", R"({"__type":"animation_clip_desc","name":"Run","duration":2.0,"ticks_per_second":30.0,"channels":{}})");
	const auto sky = std::dynamic_pointer_cast<scn::skybox_desc>(require_loaded<desc::desc_base>(sky_tag).get_sync());
	BOOST_REQUIRE(sky && sky->material.is_valid());
	BOOST_REQUIRE(desc::is_override_tag(sky->material.get_tag().view()));

	scn::world_context source{ 1, "main" };
	entt::registry& from = source.state();
	const auto named = [&](std::string_view name) {
		const entt::entity entity = from.create();
		from.emplace<scn::name_component>(entity, scn::name_component{ std::string{ name } });
		return entity;
	};
	from.emplace<scn::material_desc_component>(named("Tagged"), scn::material_desc_component{ require_loaded<scn::material_desc>(material_tag), false });
	from.emplace<scn::material_desc_component>(named("Inline"), scn::material_desc_component{ sky->material, false });
	scn::animation_collection_component clips;
	clips.clips.push_back(require_loaded<scn::animation_clip_desc>(clip_tag));
	from.emplace<scn::animation_collection_component>(named("Animator"), clips);
	scn::prefab_instance instance;
	instance.source_prefab = tag_of("p.prefab.desc");
	instance.local_overrides.name = "Root";
	scn::prefab_desc::prefab_node child;
	child.name = "Child";
	child.node_id = 0x42;
	instance.local_overrides.children.push_back(child);
	from.emplace<scn::prefab_instance>(named("Instance"), instance);

	scn::snapshot_report report;
	const json::object snapshot = scn::capture_world(source, layer, report);
	scn::world_context target{ 1, "main" };
	entt::registry& to = target.state();
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), report));
	BOOST_TEST(report.partial.empty());
	pump();

	const auto& tagged = to.get<scn::material_desc_component>(find_named(to, "Tagged"));
	BOOST_TEST((tagged.mlt_desc.get_tag() == material_tag));
	BOOST_TEST(tagged.dirty); // compiled again
	BOOST_REQUIRE(tagged.mlt_desc.get_sync());
	BOOST_TEST((tagged.mlt_desc.get_sync()->queue == scn::pass_queue::TRANSPARENT));

	const auto& inline_material = to.get<scn::material_desc_component>(find_named(to, "Inline"));
	BOOST_TEST(desc::is_override_tag(inline_material.mlt_desc.get_tag().view()));
	BOOST_TEST(inline_material.dirty);
	BOOST_REQUIRE(inline_material.mlt_desc.get_sync());
	BOOST_TEST(inline_material.mlt_desc.get_sync()->vertex_shader.string() == "res://shaders/sky.vert");

	const auto& restored_clips = to.get<scn::animation_collection_component>(find_named(to, "Animator")).clips;
	BOOST_REQUIRE_EQUAL(restored_clips.size(), 1u);
	BOOST_TEST((restored_clips[0].get_tag() == clip_tag));

	const auto& restored_instance = to.get<scn::prefab_instance>(find_named(to, "Instance"));
	BOOST_TEST((restored_instance.source_prefab == instance.source_prefab));
	BOOST_TEST(restored_instance.local_overrides.name == "Root");
	BOOST_REQUIRE_EQUAL(restored_instance.local_overrides.children.size(), 1u);
	BOOST_TEST(restored_instance.local_overrides.children[0].name == "Child");
}

BOOST_AUTO_TEST_CASE(TheFirstFrameAfterARestoreComputesTheSameTransforms)
{
	ecs::system_factory systems;
	scn::init_transform_system(systems);
	const auto world_of = [&](scn::context_set& set) -> scn::world_context& {
		auto& world = set.create_context("main", 0);
		set.add_systems(world, { "scn::depth_system", "scn::transform_system" }, systems);
		return world;
	};
	scn::context_set source_set("play", nullptr);
	scn::world_context& source = world_of(source_set);
	spawn(source.state());
	source_set.update(std::chrono::duration<float>(0.016f));

	scn::snapshot_report report;
	const json::object snapshot = scn::capture_world(source, layer, report);
	scn::context_set target_set("play", nullptr);
	scn::world_context& target = world_of(target_set);
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer, read_context(), report));
	target_set.update(std::chrono::duration<float>(0.016f));

	const entt::registry& from = source.state();
	entt::registry& to = target.state();
	std::size_t checked = 0;
	for (const auto [entity, world] : from.view<scn::world_transform>().each()) {
		const entt::entity copy = counterpart(from, entity, to);
		BOOST_REQUIRE(copy != entt::null);
		BOOST_REQUIRE(to.all_of<scn::world_transform>(copy));
		BOOST_TEST(std::memcmp(&to.get<scn::world_transform>(copy).world, &world.world, sizeof(glm::mat4)) == 0);
		if (const auto* depth = from.try_get<scn::depth_level>(entity)) {
			BOOST_REQUIRE(to.all_of<scn::depth_level>(copy));
			BOOST_TEST(to.get<scn::depth_level>(copy).value == depth->value);
		}
		++checked;
	}
	BOOST_TEST(checked >= 5u);
	// What the render extractor draws: a mesh with its geometry, transform and the renderable mark.
	const entt::entity body = find_named(to, "Body");
	BOOST_TEST((to.all_of<scn::renderable, scn::mesh_component, scn::world_transform>(body)));
}

BOOST_AUTO_TEST_CASE(OnlyAnEmptyWorldTakesASnapshot)
{
	scn::world_context source{ 1, "main" };
	scn::snapshot_report report;
	const json::object snapshot = scn::capture_world(source, layer, report);

	scn::world_context busy{ 1, "main" };
	(void)busy.state().create();
	BOOST_TEST(!scn::restore_world(busy, snapshot, layer, read_context(), report));

	scn::world_context empty{ 1, "main" };
	json::object other = snapshot;
	other["__type"] = "level_desc";
	BOOST_TEST(!scn::restore_world(empty, other, layer, read_context(), report));
}

BOOST_AUTO_TEST_SUITE_END()
