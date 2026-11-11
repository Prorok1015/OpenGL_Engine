#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_command_buffer.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_hot_reload_manager.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_scene_layer.h>
#include <snk/scene/scn_schema_components.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <memory>
#include <string>
#include <utility>

// Components without a C++ type (#606, DESC_SCHEMA.md section 3): registered in a layer from a schema alone, spawned
// from a level, saved by snapshots, and gone with their pools when the registration goes.

namespace {

constexpr const char* k_health_schema = R"json({
	"name": "mod.health", "kind": "component",
	"fields": [
		{ "name": "hp", "kind": "f32", "default": 100, "range": [0, 1000] },
		{ "name": "armor", "kind": "i32", "default": 1 },
		{ "name": "label", "kind": "string", "default": "unit" }
	]
})json";

constexpr const char* k_health_v2_schema = R"json({
	"name": "mod.health", "kind": "component",
	"fields": [
		{ "name": "label", "kind": "string", "default": "v2" },
		{ "name": "hp", "kind": "f32", "default": 50 }
	]
})json";

constexpr const char* k_level = R"json({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0",
	 "components":{"h":{"__type":"mod.health","hp":42,"label":"boss"}}}]})json";

desc::type_schema schema_from(const char* text)
{
	return desc::parse_type_schema(boost::json::parse(text).as_object());
}

struct schema_fixture : test_util::scene_env {
	schema_fixture()
		: hot(std::make_shared<scn::hot_reload_manager>(*assembler, *rs))
		, mgr(*rs, *assembler, sf, hot)
	{
		scn::register_engine_components(mgr.catalog());
		layer = &mgr.create_layer();
		mgr.set_game_layer(layer);
	}

	scn::schema_component_registration register_health(const char* text = k_health_schema)
	{
		return scn::register_schema_component(mgr, *layer, schema_from(text));
	}

	bool nothing_registered(std::string_view name) const
	{
		return !layer->components().find(name) && !layer->catalog().find(name) && !layer->descs().is_registered(name)
			&& !layer->descs().find_schema(name);
	}

	// The world's single entity with the health component.
	entt::entity health_holder(const entt::registry& reg) const
	{
		const auto* pool = reg.storage<desc::dyn_blob>(scn::schema_component_id("mod.health"));
		return pool && pool->size() == 1u ? *pool->begin() : entt::null;
	}

	boost::json::object value_of(const entt::registry& reg, entt::entity entity) const
	{
		const scn::component_entry* entry = layer->catalog().find("mod.health");
		boost::json::object out;
		if (entry) {
			entry->write(*entry->schema, reg, entity, out);
		}
		return out;
	}

	desc::read_context read_context() const
	{
		desc::read_context ctx;
		ctx.system = &layer->descs();
		return ctx;
	}

	std::shared_ptr<scn::hot_reload_manager> hot;
	ecs::system_factory sf;
	scn::context_manager mgr;
	scn::scene_layer* layer = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(scn_schema_components_tests, schema_fixture)

BOOST_AUTO_TEST_CASE(a_schema_alone_registers_a_component)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	const entt::id_type id = scn::schema_component_id("mod.health");
	BOOST_TEST(health.id() == id);
	BOOST_TEST(health.name() == "mod.health");
	BOOST_TEST(id == entt::hashed_string{ "mod.health" }.value());

	const scn::component_entry* entry = layer->catalog().find("mod.health");
	BOOST_REQUIRE((entry != nullptr));
	BOOST_TEST((layer->catalog().find(id) == entry));
	BOOST_TEST(scn::is_schema_component(*entry));
	BOOST_TEST(entry->saved());
	BOOST_TEST((layer->components().find("mod.health") != nullptr));
	BOOST_TEST(layer->descs().is_registered("mod.health"));
	BOOST_TEST(!descs->is_registered("mod.health")); // the base desc system is left alone
}

BOOST_AUTO_TEST_CASE(a_taken_name_or_id_is_refused_and_nothing_is_left_behind)
{
	auto first = register_health();
	BOOST_REQUIRE(static_cast<bool>(first));
	BOOST_TEST(!register_health()); // the same name again
	BOOST_TEST((layer->catalog().find("mod.health") != nullptr)); // the first one is untouched

	// An engine component (C++ type, in the base catalog).
	auto engine = scn::register_schema_component(mgr, *layer, schema_from(R"json({
		"name": "scn.name", "kind": "component", "fields": [ { "name": "v", "kind": "i32" } ] })json"));
	BOOST_TEST(!engine);

	// A desc type of the same name.
	auto desc_type = scn::register_schema_component(mgr, *layer, schema_from(R"json({
		"name": "level_desc", "kind": "component", "fields": [ { "name": "v", "kind": "i32" } ] })json"));
	BOOST_TEST(!desc_type);

	// A schema that describes no component.
	auto plain = scn::register_schema_component(mgr, *layer, schema_from(R"json({
		"name": "mod.plain", "kind": "struct", "fields": [ { "name": "v", "kind": "i32" } ] })json"));
	BOOST_TEST(!plain);
	BOOST_TEST(nothing_registered("mod.plain"));
}

BOOST_AUTO_TEST_CASE(a_level_spawns_the_component_with_its_values)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();

	auto* world = mgr.level_set().find("w0");
	BOOST_REQUIRE((world != nullptr));
	const entt::entity holder = health_holder(world->state());
	BOOST_REQUIRE(holder != entt::null);
	const scn::component_entry& entry = *layer->catalog().find("mod.health");
	BOOST_TEST(scn::holds(entry, world->state(), holder));
	BOOST_TEST((scn::find_schema_component(world->state(), holder, health.id()) != nullptr));

	const boost::json::object value = value_of(world->state(), holder);
	BOOST_TEST(value.at("hp").to_number<double>() == 42.0);
	BOOST_TEST(value.at("armor").to_number<std::int64_t>() == 1);
	BOOST_TEST((value.at("label").as_string() == "boss"));
}

BOOST_AUTO_TEST_CASE(the_catalog_reads_and_writes_the_component)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	const scn::component_entry& entry = *layer->catalog().find("mod.health");

	entt::registry reg;
	const entt::entity entity = reg.create();
	BOOST_TEST(!scn::holds(entry, reg, entity));
	BOOST_TEST(entry.read(*entry.schema, reg, entity, boost::json::parse(R"json({"hp":7,"armor":3,"label":"a"})json").as_object(), read_context()));
	BOOST_TEST(scn::holds(entry, reg, entity));

	boost::json::object out;
	BOOST_TEST(entry.write(*entry.schema, reg, entity, out));
	BOOST_TEST(out.at("hp").to_number<double>() == 7.0);
	BOOST_TEST(out.at("armor").to_number<std::int64_t>() == 3);
	BOOST_TEST((out.at("label").as_string() == "a"));

	// A value that does not fit its field: read, but reported.
	BOOST_TEST(!entry.read(*entry.schema, reg, entity, boost::json::parse(R"json({"hp":"lots"})json").as_object(), read_context()));
	BOOST_TEST(scn::holds(entry, reg, entity));

	const entt::entity other = reg.create();
	boost::json::object none;
	BOOST_TEST(!entry.write(*entry.schema, reg, other, none));
}

BOOST_AUTO_TEST_CASE(a_stray_pool_is_named_by_its_id)
{
	entt::registry reg;
	const entt::id_type id = scn::schema_component_id("mod.stray");
	scn::emplace_schema_component(reg, reg.create(), id, desc::dyn_blob{});
	bool named = false;
	for (const scn::unregistered_storage& stray : scn::unregistered_storages(reg, layer->catalog())) {
		if (stray.id == id) {
			named = stray.type.find(std::to_string(id)) != std::string::npos;
		}
	}
	BOOST_TEST(named);
}

BOOST_AUTO_TEST_CASE(a_snapshot_keeps_the_component)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	auto& source = mgr.level_set().get("w0");

	scn::snapshot_report captured;
	const boost::json::object snapshot = scn::capture_world(source, layer->catalog(), captured);
	BOOST_TEST(captured.unsaved.empty());

	scn::world_context target{ 1, "copy" };
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer->catalog(), read_context(), restored));
	BOOST_TEST(restored.unknown.empty());
	const entt::entity holder = health_holder(target.state());
	BOOST_REQUIRE(holder != entt::null);
	const boost::json::object value = value_of(target.state(), holder);
	BOOST_TEST(value.at("hp").to_number<double>() == 42.0);
	BOOST_TEST((value.at("label").as_string() == "boss"));
}

BOOST_AUTO_TEST_CASE(dropping_the_registration_clears_every_world_and_frees_the_name)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	const entt::id_type id = health.id();
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	auto& level_world = mgr.level_set().get("w0");

	mgr.create_set("other", layer);
	auto& other_world = mgr.create_context("other", "w1", scn::context_kind::runtime);
	scn::emplace_schema_component(other_world.state(), other_world.state().create(), id, desc::dyn_blob{});

	health.reset();
	BOOST_TEST(!health);
	BOOST_TEST((health_holder(level_world.state()) == entt::null));
	const auto* other_pool = std::as_const(other_world.state()).storage<desc::dyn_blob>(id);
	BOOST_TEST((other_pool == nullptr || other_pool->empty()));
	BOOST_TEST(nothing_registered("mod.health"));

	// The name is free for another layout.
	auto v2 = register_health(k_health_v2_schema);
	BOOST_REQUIRE(static_cast<bool>(v2));
	BOOST_TEST(v2.id() == id);
	entt::registry reg;
	const entt::entity entity = reg.create();
	const scn::component_entry& entry = *layer->catalog().find("mod.health");
	BOOST_TEST(entry.read(*entry.schema, reg, entity, boost::json::object{}, read_context()));
	const boost::json::object value = value_of(reg, entity);
	BOOST_TEST((value.at("label").as_string() == "v2"));
	BOOST_TEST(!value.contains("armor"));
}

BOOST_AUTO_TEST_CASE(the_layer_drops_a_kept_registration_after_its_worlds)
{
	layer->keep(register_health());
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	BOOST_REQUIRE(health_holder(mgr.level_set().get("w0").state()) != entt::null);

	auto& next = mgr.create_layer();
	mgr.set_game_layer(&next);
	mgr.destroy_layer(*layer);
	layer = &next;
	BOOST_TEST(nothing_registered("mod.health"));
}

BOOST_AUTO_TEST_CASE(sandbox_commands_without_meta_are_dropped_not_crashed)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	auto& world = mgr.level_set().get("w0");
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	const auto spawned = sandbox.create();
	sandbox.storage<desc::dyn_blob>(health.id()).emplace(spawned);

	world.apply_commands();
	BOOST_TEST((health_holder(world.state()) != entt::null)); // the level's entity keeps its own; PR-2 moves the rest
}

BOOST_AUTO_TEST_SUITE_END()
