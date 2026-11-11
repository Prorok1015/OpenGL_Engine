#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/desc/desc_dynamic.h>
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
#include <format>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
		{ "name": "hp", "kind": "f32", "default": 50 },
		{ "name": "shield", "kind": "i32", "default": 3 }
	]
})json";

constexpr const char* k_inner_schema = R"json({
	"name": "mod.inner", "kind": "struct", "fields": [ { "name": "target", "kind": "entity" } ]
})json";

// Entities in a nested structure and in an array.
constexpr const char* k_deep_schema = R"json({
	"name": "mod.deep", "kind": "component",
	"fields": [
		{ "name": "n", "kind": "i32" },
		{ "name": "inner", "kind": "structure", "ref_type": "mod.inner" },
		{ "name": "many", "kind": "array", "element": "entity" }
	]
})json";

constexpr const char* k_link_schema = R"json({
	"name": "mod.link", "kind": "component",
	"fields": [ { "name": "n", "kind": "i32" }, { "name": "target", "kind": "entity" } ]
})json";

// Reads entity numbers as they are: the raw ids a test puts into a blob.
struct identity_remap final : desc::entity_remap {
	std::optional<std::uint32_t> find(std::uint32_t saved) const override { return saved; }
};

constexpr const char* k_level = R"json({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0",
	 "components":{"h":{"__type":"mod.health","hp":42,"label":"boss"}}}]})json";

desc::type_schema schema_from(const char* text)
{
	return desc::parse_type_schema(boost::json::parse(text).as_object());
}

// A dynamic desc of `text`'s layout, as a cached desc read under some registration of the name would be. Its type is
// set as desc_system::create_instance sets it (set_type is not public).
struct typed_dynamic_desc : desc::dynamic_desc {
	explicit typed_dynamic_desc(std::shared_ptr<const desc::schema_binding> binding)
		: desc::dynamic_desc(std::move(binding))
	{
		set_type(schema().name);
	}
};

std::shared_ptr<desc::dynamic_desc> desc_of(const char* text, const desc::schema_registry& resolve)
{
	return std::make_shared<typed_dynamic_desc>(desc::bind_runtime(schema_from(text), resolve));
}

desc::type_schema schema_in_code(std::string name)
{
	desc::type_schema schema;
	schema.name = std::move(name);
	schema.kind = desc::schema_kind::component;
	schema.is_component = true;
	desc::field_schema field;
	field.name = "v";
	field.key = "/v";
	field.kind = desc::field_kind::i32;
	field.default_value = 5;
	schema.fields.push_back(std::move(field));
	return schema;
}

struct log_capture {
	log_capture()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) warnings.push_back(std::format("{}: {}", category, message));
		});
	}

	std::size_t count(std::string_view part) const
	{
		std::size_t found = 0;
		for (const auto& w : warnings) {
			found += w.find(part) != std::string::npos ? 1u : 0u;
		}
		return found;
	}

	std::vector<std::string> warnings;
	engine::log_sink_handle sink; // last: removed first
};

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
			&& !layer->descs().find_schema(name) && !layer->assembler().has_spawner(name);
	}

	// The world's single entity with the health component.
	entt::entity health_holder(const entt::registry& reg) const
	{
		const auto* pool = reg.storage<desc::dyn_blob>(scn::schema_component_id("mod.health"));
		// Through the entity set: the storage's own iterator walks the blobs.
		return pool && pool->size() == 1u ? *static_cast<const entt::sparse_set&>(*pool).begin() : entt::null;
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

	// A mod.link blob of `n` naming the raw entity `target` (null: none).
	desc::dyn_blob link_blob(int n, std::optional<std::uint32_t> target) const
	{
		const desc::schema_binding& binding = *layer->catalog().find("mod.link")->schema;
		desc::dyn_blob blob = desc::make_blob(binding);
		desc::dyn_ref ref{ &blob, 0 };
		boost::json::object in{ { "n", n } };
		in["target"] = target ? boost::json::value(static_cast<std::uint64_t>(*target)) : boost::json::value(nullptr);
		const identity_remap identity;
		desc::read_context ctx;
		ctx.entities = &identity;
		desc::deserialize(binding, &ref, in, ctx);
		return blob;
	}

	// The entity of `reg` whose mod.link has `n`.
	entt::entity link_of(const entt::registry& reg, int n) const
	{
		const auto* pool = reg.storage<desc::dyn_blob>(scn::schema_component_id("mod.link"));
		if (!pool) {
			return entt::null;
		}
		const scn::component_entry& entry = *layer->catalog().find("mod.link");
		for (const entt::entity entity : static_cast<const entt::sparse_set&>(*pool)) {
			boost::json::object out;
			if (entry.write(*entry.schema, reg, entity, out) && out.at("n").to_number<std::int64_t>() == n) {
				return entity;
			}
		}
		return entt::null;
	}

	boost::json::value target_of(const entt::registry& reg, entt::entity entity) const
	{
		const scn::component_entry& entry = *layer->catalog().find("mod.link");
		boost::json::object out;
		entry.write(*entry.schema, reg, entity, out);
		return out.at("target");
	}

	static boost::json::value raw(entt::entity entity) { return boost::json::value(static_cast<std::uint64_t>(entt::to_integral(entity))); }

	scn::schema_component_registration register_deep()
	{
		layer->descs().register_schema(schema_from(k_inner_schema));
		return scn::register_schema_component(mgr, *layer,
			desc::parse_type_schema(boost::json::parse(k_deep_schema).as_object(), &layer->descs().schemas()));
	}

	const desc::schema_binding& deep_binding() const { return *layer->catalog().find("mod.deep")->schema; }

	// A mod.deep blob of `n` whose entity fields hold the raw ids of `in` as they are.
	desc::dyn_blob deep_blob(const boost::json::object& in) const
	{
		desc::dyn_blob blob = desc::make_blob(deep_binding());
		desc::dyn_ref ref{ &blob, 0 };
		const identity_remap identity;
		desc::read_context ctx;
		ctx.entities = &identity;
		desc::deserialize(deep_binding(), &ref, in, ctx);
		return blob;
	}

	boost::json::object deep_of(const entt::registry& reg, int n) const
	{
		const auto* pool = reg.storage<desc::dyn_blob>(scn::schema_component_id("mod.deep"));
		if (pool) {
			for (const entt::entity entity : static_cast<const entt::sparse_set&>(*pool)) {
				desc::dyn_ref ref{ const_cast<desc::dyn_blob*>(&pool->get(entity)), 0 };
				boost::json::object out;
				desc::serialize(deep_binding(), &ref, out);
				if (out.at("n").to_number<std::int64_t>() == n) {
					out["__entity"] = raw(entity);
					return out;
				}
			}
		}
		return {};
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
	BOOST_TEST(layer->components().find("mod.health").has_value());
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

	// Only the id: another name holds hashed_string("mod.y").
	BOOST_REQUIRE(layer->components().add("mod.x", scn::schema_component_id("mod.y")));
	auto by_id = scn::register_schema_component(mgr, *layer, schema_from(R"json({
		"name": "mod.y", "kind": "component", "fields": [ { "name": "v", "kind": "i32" } ] })json"));
	BOOST_TEST(!by_id);
	BOOST_TEST(nothing_registered("mod.y"));
	BOOST_TEST(layer->components().find("mod.x").has_value());

	// A version its migrations do not lead to (none registered: version 1).
	auto versioned = scn::register_schema_component(mgr, *layer, schema_from(R"json({
		"name": "mod.versioned", "version": 2, "kind": "component", "fields": [ { "name": "v", "kind": "i32" } ] })json"));
	BOOST_TEST(!versioned);
	BOOST_TEST(nothing_registered("mod.versioned"));
}

BOOST_AUTO_TEST_CASE(a_schema_built_in_code_gets_its_layout_or_leaves_nothing_behind)
{
	auto built = scn::register_schema_component(mgr, *layer, schema_in_code("mod.built"));
	BOOST_REQUIRE(static_cast<bool>(built));
	const scn::component_entry& entry = *layer->catalog().find("mod.built");
	entt::registry reg;
	const entt::entity entity = reg.create();
	BOOST_TEST(entry.read(*entry.schema, reg, entity, boost::json::object{}, read_context()));
	boost::json::object out;
	BOOST_TEST(entry.write(*entry.schema, reg, entity, out));
	BOOST_TEST(out.at("v").to_number<std::int64_t>() == 5);

	// Its layout cannot be computed (the nested schema is missing): the throw is caught and rolled back.
	desc::type_schema broken = schema_in_code("mod.broken");
	desc::field_schema nested;
	nested.name = "s";
	nested.key = "/s";
	nested.kind = desc::field_kind::structure;
	nested.ref_type = "mod.missing";
	broken.fields.push_back(std::move(nested));
	auto refused = scn::register_schema_component(mgr, *layer, std::move(broken));
	BOOST_TEST(!refused);
	BOOST_TEST(nothing_registered("mod.broken"));
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

BOOST_AUTO_TEST_CASE(a_desc_of_another_layout_is_not_spawned)
{
	auto health = register_health(k_health_v2_schema);
	BOOST_REQUIRE(static_cast<bool>(health));
	entt::registry reg;
	const entt::entity entity = reg.create();

	const auto stale = desc_of(k_health_schema, layer->descs().schemas());
	layer->assembler().spawn_from_desc(reg, entity, *stale);
	BOOST_TEST((scn::find_schema_component(reg, entity, health.id()) == nullptr));

	const auto current = desc_of(k_health_v2_schema, layer->descs().schemas());
	layer->assembler().spawn_from_desc(reg, entity, *current);
	BOOST_TEST((scn::find_schema_component(reg, entity, health.id()) != nullptr));
}

BOOST_AUTO_TEST_CASE(a_blob_of_another_size_is_not_read)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	const scn::component_entry& entry = *layer->catalog().find("mod.health");
	entt::registry reg;
	const entt::entity entity = reg.create();
	scn::emplace_schema_component(reg, entity, health.id(), desc::dyn_blob{});
	boost::json::object out;
	BOOST_TEST(!entry.write(*entry.schema, reg, entity, out));
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

BOOST_AUTO_TEST_CASE(the_next_layer_of_a_hot_reload_keeps_its_components)
{
	scn::scene_layer& previous = *layer;
	previous.keep(register_health());
	auto& next = mgr.create_layer();
	// A sibling, not a child: the name is free in it.
	auto next_health = scn::register_schema_component(mgr, next, schema_from(k_health_schema));
	BOOST_REQUIRE(static_cast<bool>(next_health));

	mgr.set_game_layer(&next);
	layer = &next;
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	auto& world = mgr.level_set().get("w0");
	BOOST_REQUIRE((health_holder(world.state()) != entt::null));

	mgr.destroy_layer(previous); // drops the previous registration, as activate() does after the switch
	const entt::entity holder = health_holder(world.state());
	BOOST_REQUIRE((holder != entt::null));
	BOOST_TEST(value_of(world.state(), holder).at("hp").to_number<double>() == 42.0);
	BOOST_TEST((layer->catalog().find("mod.health") != nullptr));
}

BOOST_AUTO_TEST_CASE(a_reset_while_the_layer_lives_leaves_a_sibling_set_alone)
{
	auto& sibling = mgr.create_layer();
	auto mine = register_health();
	auto theirs = scn::register_schema_component(mgr, sibling, schema_from(k_health_schema));
	BOOST_REQUIRE(static_cast<bool>(mine));
	BOOST_REQUIRE(static_cast<bool>(theirs));

	mgr.create_set("mine", layer);
	mgr.create_set("theirs", &sibling);
	auto& my_world = mgr.create_context("mine", "w", scn::context_kind::runtime);
	auto& their_world = mgr.create_context("theirs", "w", scn::context_kind::runtime);
	scn::emplace_schema_component(my_world.state(), my_world.state().create(), mine.id(), desc::dyn_blob{});
	scn::emplace_schema_component(their_world.state(), their_world.state().create(), theirs.id(), desc::dyn_blob{});

	mine.reset();
	const auto* my_pool = std::as_const(my_world.state()).storage<desc::dyn_blob>(theirs.id());
	BOOST_TEST((my_pool == nullptr || my_pool->empty()));
	const auto* their_pool = std::as_const(their_world.state()).storage<desc::dyn_blob>(theirs.id());
	BOOST_REQUIRE((their_pool != nullptr));
	BOOST_TEST(their_pool->size() == 1u);
	BOOST_TEST((sibling.catalog().find("mod.health") != nullptr));

	theirs.reset();
	mgr.destroy_layer(sibling);
}

BOOST_AUTO_TEST_CASE(sandbox_commands_without_meta_are_dropped_not_crashed)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	auto& world = mgr.level_set().get("w0");
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	// An id no other test drops: the warning is given once per id for the process.
	const entt::id_type id = scn::schema_component_id("mod.sandbox_only");
	const std::string id_text = std::to_string(id);
	log_capture log;

	sandbox.storage<desc::dyn_blob>(id).emplace(sandbox.create());
	world.apply_commands();
	BOOST_TEST(log.count(id_text) == 1u);
	BOOST_TEST((health_holder(world.state()) != entt::null)); // the level's entity keeps its own; PR-2 moves the rest

	sandbox.storage<desc::dyn_blob>(id).emplace(sandbox.create());
	world.apply_commands();
	BOOST_TEST(log.count(id_text) == 1u); // once per id, not every frame
}

BOOST_AUTO_TEST_CASE(an_entity_field_without_a_value_names_no_entity)
{
	auto link = register_health(k_link_schema);
	BOOST_REQUIRE(static_cast<bool>(link));
	const desc::schema_binding& binding = *layer->catalog().find("mod.link")->schema;
	desc::dyn_blob blob = desc::make_blob(binding);
	desc::dyn_ref ref{ &blob, 0 };
	boost::json::object out;
	desc::serialize(binding, &ref, out);
	BOOST_TEST(out.at("target").is_null()); // not entity 0, a live one in any world
}

BOOST_AUTO_TEST_CASE(a_snapshot_maps_the_entity_fields_to_the_restored_entities)
{
	auto link = register_health(k_link_schema);
	BOOST_REQUIRE(static_cast<bool>(link));
	scn::world_context source{ 1, "main" };
	entt::registry& from = source.state();
	const entt::entity pointee = from.create();
	const entt::entity pointer = from.create();
	scn::emplace_schema_component(from, pointee, link.id(), link_blob(2, std::nullopt));
	scn::emplace_schema_component(from, pointer, link.id(), link_blob(1, entt::to_integral(pointee)));

	scn::snapshot_report captured;
	const boost::json::object snapshot = scn::capture_world(source, layer->catalog(), captured);
	scn::world_context target{ 1, "copy" };
	entt::registry& to = target.state();
	static_cast<void>(to.create()); // shifts the ids: a field keeping the raw id would name another entity
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer->catalog(), read_context(), restored));

	const entt::entity restored_pointer = link_of(to, 1);
	const entt::entity restored_pointee = link_of(to, 2);
	BOOST_REQUIRE((restored_pointer != entt::null));
	BOOST_REQUIRE((restored_pointee != entt::null));
	BOOST_TEST((target_of(to, restored_pointer) == raw(restored_pointee)));
	BOOST_TEST(target_of(to, restored_pointee).is_null());
}

BOOST_AUTO_TEST_CASE(sandbox_commands_move_the_component_and_map_its_entities)
{
	auto link = register_health(k_link_schema);
	BOOST_REQUIRE(static_cast<bool>(link));
	mgr.create_set("s", layer);
	auto& world = mgr.create_context("s", "w", scn::context_kind::runtime);
	static_cast<void>(world.state().create()); // the world's ids differ from the sandbox's
	static_cast<void>(world.state().create());
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	const auto first = sandbox.create();
	const auto second = sandbox.create();
	auto& pool = sandbox.storage<desc::dyn_blob>(link.id());
	pool.emplace(first, link_blob(1, entt::to_integral(second)));
	// A raw id the sandbox does not have: it may be another entity of the world, so it is dropped.
	pool.emplace(second, link_blob(2, 4000u));
	log_capture log;

	world.apply_commands();
	const entt::entity moved_first = link_of(world.state(), 1);
	const entt::entity moved_second = link_of(world.state(), 2);
	BOOST_REQUIRE((moved_first != entt::null));
	BOOST_REQUIRE((moved_second != entt::null));
	BOOST_TEST((target_of(world.state(), moved_first) == raw(moved_second)));
	BOOST_TEST(target_of(world.state(), moved_second).is_null());
	BOOST_TEST(log.count(std::to_string(link.id())) == 0u); // moved, not dropped
	BOOST_TEST(sandbox.storage<desc::dyn_blob>(link.id()).empty());
}

BOOST_AUTO_TEST_CASE(a_sandbox_blob_of_another_layout_is_dropped)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	mgr.create_set("s", layer);
	auto& world = mgr.create_context("s", "w", scn::context_kind::runtime);
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	sandbox.storage<desc::dyn_blob>(health.id()).emplace(sandbox.create(), desc::dyn_blob{});
	world.apply_commands();
	const auto* pool = std::as_const(world.state()).storage<desc::dyn_blob>(health.id());
	BOOST_TEST((pool == nullptr || pool->empty()));
}

BOOST_AUTO_TEST_CASE(a_component_written_by_no_layout_is_reported_unsaved)
{
	auto health = register_health();
	BOOST_REQUIRE(static_cast<bool>(health));
	scn::world_context source{ 1, "main" };
	scn::emplace_schema_component(source.state(), source.state().create(), health.id(), desc::dyn_blob{});
	scn::snapshot_report captured;
	const boost::json::object snapshot = scn::capture_world(source, layer->catalog(), captured);
	BOOST_TEST(captured.unsaved.count("mod.health") == 1u);
	BOOST_TEST(boost::json::serialize(snapshot).find("mod.health") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(entities_in_a_nested_structure_and_an_array_follow_a_snapshot)
{
	auto deep = register_deep();
	BOOST_REQUIRE(static_cast<bool>(deep));
	scn::world_context source{ 1, "main" };
	entt::registry& from = source.state();
	const entt::entity pointee = from.create();
	const entt::entity pointer = from.create();
	scn::emplace_schema_component(from, pointee, deep.id(), deep_blob(boost::json::object{ { "n", 2 } }));
	boost::json::object in{ { "n", 1 } };
	in["inner"] = boost::json::object{ { "target", raw(pointee) } };
	in["many"] = boost::json::array{ raw(pointee), 999999 };
	scn::emplace_schema_component(from, pointer, deep.id(), deep_blob(in));

	scn::snapshot_report captured;
	const boost::json::object snapshot = scn::capture_world(source, layer->catalog(), captured);
	scn::world_context target{ 1, "copy" };
	static_cast<void>(target.state().create());
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, layer->catalog(), read_context(), restored));

	const boost::json::object restored_pointer = deep_of(target.state(), 1);
	const boost::json::object restored_pointee = deep_of(target.state(), 2);
	BOOST_REQUIRE(!restored_pointer.empty());
	BOOST_REQUIRE(!restored_pointee.empty());
	BOOST_TEST((restored_pointer.at("inner").as_object().at("target") == restored_pointee.at("__entity")));
	const boost::json::array& many = restored_pointer.at("many").as_array();
	BOOST_REQUIRE(many.size() == 2u);
	BOOST_TEST((many[0] == restored_pointee.at("__entity")));
	BOOST_TEST(many[1].is_null()); // not in the snapshot
}

BOOST_AUTO_TEST_CASE(entities_in_a_nested_structure_and_an_array_follow_the_sandbox)
{
	auto deep = register_deep();
	BOOST_REQUIRE(static_cast<bool>(deep));
	mgr.create_set("s", layer);
	auto& world = mgr.create_context("s", "w", scn::context_kind::runtime);
	static_cast<void>(world.state().create());
	static_cast<void>(world.state().create());
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	const auto first = sandbox.create();
	const auto second = sandbox.create();
	const boost::json::value second_raw(static_cast<std::uint64_t>(entt::to_integral(second)));
	boost::json::object in{ { "n", 1 } };
	in["inner"] = boost::json::object{ { "target", second_raw } };
	in["many"] = boost::json::array{ second_raw, 4000 };
	auto& pool = sandbox.storage<desc::dyn_blob>(deep.id());
	pool.emplace(first, deep_blob(in));
	pool.emplace(second, deep_blob(boost::json::object{ { "n", 2 } }));

	world.apply_commands();
	const boost::json::object moved_first = deep_of(world.state(), 1);
	const boost::json::object moved_second = deep_of(world.state(), 2);
	BOOST_REQUIRE(!moved_first.empty());
	BOOST_REQUIRE(!moved_second.empty());
	BOOST_TEST((moved_first.at("inner").as_object().at("target") == moved_second.at("__entity")));
	const boost::json::array& many = moved_first.at("many").as_array();
	BOOST_REQUIRE(many.size() == 2u);
	BOOST_TEST((many[0] == moved_second.at("__entity")));
	BOOST_TEST(many[1].is_null()); // not a sandbox entity
}

BOOST_AUTO_TEST_CASE(a_sandbox_blob_of_another_layout_and_size_is_dropped)
{
	auto link = register_health(k_link_schema);
	BOOST_REQUIRE(static_cast<bool>(link));
	mgr.create_set("s", layer);
	auto& world = mgr.create_context("s", "w", scn::context_kind::runtime);
	if (!world.state().ctx().contains<ecs::entity_spawner>()) {
		world.state().ctx().emplace<ecs::entity_spawner>();
	}
	auto& sandbox = world.state().ctx().get<ecs::entity_spawner>();
	// An older mod.link with one more field: a real blob of make_blob, larger than the registered layout.
	const auto older = desc::bind_runtime(schema_from(R"json({
		"name": "mod.link", "kind": "component",
		"fields": [ { "name": "n", "kind": "i32" }, { "name": "target", "kind": "entity" }, { "name": "extra", "kind": "u64" } ] })json"),
		layer->descs().schemas());
	desc::dyn_blob blob = desc::make_blob(*older);
	BOOST_REQUIRE(blob.bytes.size() != link_blob(1, std::nullopt).bytes.size());
	sandbox.storage<desc::dyn_blob>(link.id()).emplace(sandbox.create(), std::move(blob));

	world.apply_commands();
	const auto* pool = std::as_const(world.state()).storage<desc::dyn_blob>(link.id());
	BOOST_TEST((pool == nullptr || pool->empty()));
}

BOOST_AUTO_TEST_CASE(an_entity_number_is_not_read_without_an_entity_map)
{
	auto deep = register_deep();
	BOOST_REQUIRE(static_cast<bool>(deep));
	desc::dyn_blob blob = desc::make_blob(deep_binding());
	desc::dyn_ref ref{ &blob, 0 };
	boost::json::object in{ { "n", 1 } };
	in["inner"] = boost::json::object{ { "target", 5 } };
	in["many"] = boost::json::array{ 5 };
	desc::deserialize(deep_binding(), &ref, in, read_context());
	boost::json::object out;
	desc::serialize(deep_binding(), &ref, out);
	BOOST_TEST(out.at("n").to_number<std::int64_t>() == 1);
	BOOST_TEST(out.at("inner").as_object().at("target").is_null());
	BOOST_TEST(!(out.contains("many") && out.at("many").is_array() && !out.at("many").as_array().empty()));
}

BOOST_AUTO_TEST_CASE(copying_an_entity_array_keeps_its_entities)
{
	auto deep = register_deep();
	BOOST_REQUIRE(static_cast<bool>(deep));
	boost::json::object in{ { "n", 1 } };
	in["many"] = boost::json::array{ 7, nullptr };
	desc::dyn_blob from = deep_blob(in);
	desc::dyn_blob to = desc::make_blob(deep_binding());
	desc::dyn_ref source{ &from, 0 };
	desc::dyn_ref copy{ &to, 0 };
	for (const desc::field_ops& ops : deep_binding().ops) {
		ops.assign(ops, &copy, &source);
	}
	boost::json::object out;
	desc::serialize(deep_binding(), &copy, out);
	const boost::json::array& many = out.at("many").as_array();
	BOOST_REQUIRE(many.size() == 2u);
	BOOST_TEST(many[0].to_number<std::uint64_t>() == 7u);
	BOOST_TEST(many[1].is_null());
}

BOOST_AUTO_TEST_CASE(remap_entities_refuses_a_blob_too_small_for_the_binding)
{
	auto link = register_health(k_link_schema);
	BOOST_REQUIRE(static_cast<bool>(link));
	desc::dyn_blob blob;
	desc::dyn_ref ref{ &blob, 0 };
	const identity_remap identity;
	BOOST_TEST(!desc::remap_entities(*layer->catalog().find("mod.link")->schema, &ref, identity));
}

BOOST_AUTO_TEST_CASE(an_array_of_structures_is_refused)
{
	layer->descs().register_schema(schema_from(k_inner_schema));
	auto listy = scn::register_schema_component(mgr, *layer, desc::parse_type_schema(boost::json::parse(R"json({
		"name": "mod.listy", "kind": "component",
		"fields": [ { "name": "items", "kind": "array", "element": "structure", "ref_type": "mod.inner" } ] })json").as_object(),
		&layer->descs().schemas()));
	BOOST_TEST(!listy);
	BOOST_TEST(nothing_registered("mod.listy"));
}

BOOST_AUTO_TEST_CASE(an_entity_field_takes_no_default_but_null)
{
	BOOST_CHECK_THROW(schema_from(R"json({ "name": "mod.d", "kind": "component",
		"fields": [ { "name": "e", "kind": "entity", "default": 5 } ] })json"), std::exception);
	BOOST_CHECK_THROW(schema_from(R"json({ "name": "mod.d", "kind": "component",
		"fields": [ { "name": "e", "kind": "array", "element": "entity", "default": [5] } ] })json"), std::exception);
	BOOST_CHECK_NO_THROW(schema_from(R"json({ "name": "mod.d", "kind": "component",
		"fields": [ { "name": "e", "kind": "entity", "default": null } ] })json"));
}

BOOST_AUTO_TEST_CASE(a_null_fits_an_entity)
{
	BOOST_TEST(desc::json_fits(desc::field_kind::entity, desc::field_kind::boolean, nullptr, boost::json::value(nullptr)));
	BOOST_TEST(desc::json_fits(desc::field_kind::array, desc::field_kind::entity, nullptr, boost::json::parse("[null, 3]")));
	BOOST_TEST(!desc::json_fits(desc::field_kind::entity, desc::field_kind::boolean, nullptr, boost::json::value("x")));
}

BOOST_AUTO_TEST_CASE(a_hot_reload_to_another_layout_keeps_the_fields_both_have)
{
	scn::scene_layer& previous = *layer;
	previous.keep(register_health());
	BOOST_REQUIRE(mgr.load_level(put("lv.desc", k_level)));
	pump();
	scn::snapshot_report captured;
	const boost::json::object snapshot = scn::capture_world(mgr.level_set().get("w0"), previous.catalog(), captured);

	auto& next = mgr.create_layer();
	auto v2 = scn::register_schema_component(mgr, next, schema_from(k_health_v2_schema));
	BOOST_REQUIRE(static_cast<bool>(v2));
	mgr.set_game_layer(&next);
	mgr.destroy_layer(previous);
	layer = &next;

	mgr.create_set("restored", &next);
	auto& target = mgr.create_context("restored", "w0", scn::context_kind::runtime);
	scn::snapshot_report restored;
	BOOST_REQUIRE(scn::restore_world(target, snapshot, next.catalog(), read_context(), restored));
	const entt::entity holder = health_holder(target.state());
	BOOST_REQUIRE((holder != entt::null));
	const boost::json::object value = value_of(target.state(), holder);
	BOOST_TEST(value.at("hp").to_number<double>() == 42.0);
	BOOST_TEST((value.at("label").as_string() == "boss"));
	BOOST_TEST(!value.contains("armor"));
	BOOST_TEST(value.at("shield").to_number<std::int64_t>() == 3); // new in v2: its default
	// A field v2 dropped is reported as removed (the value is not lost silently), not as one that no longer fits.
	BOOST_TEST(restored.removed_fields.count("mod.health") == 1u);
	BOOST_TEST(restored.partial.count("mod.health") == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
