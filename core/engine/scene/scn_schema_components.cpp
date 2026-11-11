#include <snk/scene/scn_schema_components.h>
#include <snk/common/engine_assert.h>
#include <snk/common/logger/engine_log.h>
#include <snk/desc/desc_dynamic.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_scene_layer.h>
#include <entt/core/hashed_string.hpp>
#include <entt/entity/registry.hpp>
#include <exception>
#include <format>
#include <string>
#include <typeinfo>
#include <utility>

namespace
{
	using pool_type = entt::storage_for_t<desc::dyn_blob>;

	// The entry's `contains` is never asked for a component without a C++ type (scn::holds is): it is handed no id.
	bool contains_unknown(const entt::registry&, entt::entity)
	{
		ASSERT_FAIL("the presence of a component without a C++ type is asked through scn::holds");
		return false;
	}

	// Whether blobs of `a` can be read through `b`: same bytes at the same offsets, field by field.
	bool same_layout(const desc::schema_binding& a, const desc::schema_binding& b)
	{
		if (&a == &b) {
			return true;
		}
		if (a.schema->layout != b.schema->layout || a.ops.size() != b.ops.size()) {
			return false;
		}
		for (std::size_t i = 0; i < a.ops.size(); ++i) {
			const desc::field_ops& x = a.ops[i];
			const desc::field_ops& y = b.ops[i];
			if (x.kind != y.kind || x.element != y.element || x.offset != y.offset || !x.nested != !y.nested) {
				return false;
			}
			if (x.nested && !same_layout(*x.nested, *y.nested)) {
				return false;
			}
		}
		return true;
	}

	// The key of a field that is an array of structures or of custom values, nested ones included; empty: none. Their
	// elements are opaque JSON to the runtime ops, so an entity inside one would cross registries unmapped.
	std::string unsupported_array(const desc::schema_binding& binding, const std::string& prefix = {})
	{
		for (std::size_t i = 0; i < binding.ops.size(); ++i) {
			const desc::field_ops& ops = binding.ops[i];
			const std::string key = prefix + binding.schema->fields[i].key;
			if (ops.kind == desc::field_kind::array && (ops.element == desc::field_kind::structure || ops.element == desc::field_kind::custom)) {
				return key;
			}
			if (ops.nested) {
				if (std::string found = unsupported_array(*ops.nested, key); !found.empty()) {
					return found;
				}
			}
		}
		return {};
	}

	bool write(const desc::schema_binding& schema, const entt::registry& reg, entt::entity entity, json::object& out)
	{
		const desc::dyn_blob* blob = scn::find_schema_component(reg, entity, scn::schema_component_id(schema.schema->name));
		// A blob of another size was laid out by another schema: reading it by this one's offsets overruns it.
		if (!blob || blob->bytes.size() != schema.schema->layout.size) {
			return false;
		}
		// serialize only reads through the reference.
		desc::dyn_ref ref{ const_cast<desc::dyn_blob*>(blob), 0 };
		desc::serialize(schema, &ref, out);
		return true;
	}

	bool read(const desc::schema_binding& schema, entt::registry& reg, entt::entity entity, const json::object& in, const desc::read_context& ctx)
	{
		desc::dyn_blob blob = desc::make_blob(schema);
		desc::dyn_ref ref{ &blob, 0 };
		desc::deserialize(schema, &ref, in, ctx);
		scn::emplace_schema_component(reg, entity, scn::schema_component_id(schema.schema->name), std::move(blob));
		return desc::check_override_object(*schema.schema, in).status == desc::override_status::valid;
	}

	void spawn(entt::id_type id, const desc::schema_binding& registered, entt::registry& reg, entt::entity entity, const desc::desc_base& source)
	{
		const auto* dynamic = dynamic_cast<const desc::dynamic_desc*>(&source);
		if (!dynamic) {
			egLOG_ERROR("scene", "component without a C++ type: the desc of '{}' is not a dynamic_desc", source.get_type());
			return;
		}
		// A desc read by an earlier registration of the name (still cached) lays its blob out by that schema.
		if (!same_layout(dynamic->binding(), registered)) {
			egLOG_ERROR("scene", "component without a C++ type '{}' not spawned: its desc was read by another layout of the schema", source.get_type());
			return;
		}
		scn::emplace_schema_component(reg, entity, id, dynamic->blob());
	}
}

entt::id_type scn::schema_component_id(std::string_view name)
{
	return entt::hashed_string::value(name.data(), name.size());
}

desc::dyn_blob* scn::find_schema_component(entt::registry& reg, entt::entity entity, entt::id_type id)
{
	// Through the const registry, which looks a pool up without making one.
	return const_cast<desc::dyn_blob*>(find_schema_component(std::as_const(reg), entity, id));
}

const desc::dyn_blob* scn::find_schema_component(const entt::registry& reg, entt::entity entity, entt::id_type id)
{
	const pool_type* pool = reg.storage<desc::dyn_blob>(id);
	return pool && pool->contains(entity) ? &pool->get(entity) : nullptr;
}

desc::dyn_blob& scn::emplace_schema_component(entt::registry& reg, entt::entity entity, entt::id_type id, desc::dyn_blob blob)
{
	pool_type& pool = reg.storage<desc::dyn_blob>(id);
	if (pool.contains(entity)) {
		return pool.get(entity) = std::move(blob);
	}
	return pool.emplace(entity, std::move(blob));
}

bool scn::is_schema_component(const component_entry& entry)
{
	return entry.object_type && *entry.object_type == typeid(desc::dyn_ref) && entry.contains == &contains_unknown;
}

bool scn::holds(const component_entry& entry, const entt::registry& reg, entt::entity entity)
{
	if (is_schema_component(entry)) {
		return find_schema_component(reg, entity, entry.id) != nullptr;
	}
	return entry.contains(reg, entity);
}

const scn::component_entry* scn::component_catalog::add_schema_component(std::string name, std::shared_ptr<const desc::schema_binding> schema)
{
	if (!schema || !schema->schema || !schema->object_type || *schema->object_type != typeid(desc::dyn_ref)) {
		egLOG_ERROR("scene", "component '{}' not added to the catalog: its schema is not a runtime binding", name);
		return nullptr;
	}
	component_entry entry;
	entry.id = schema_component_id(name);
	entry.name = std::move(name);
	entry.category = component_category::data;
	entry.schema = std::move(schema);
	entry.contains = &contains_unknown;
	entry.write = &write;
	entry.read = &read;
	entry.object_type = &typeid(desc::dyn_ref);
	return add_entry(std::move(entry));
}

scn::schema_component_registration::schema_component_registration(context_manager& manager, scene_layer& layer, std::string name, entt::id_type id)
	: m_manager(&manager)
	, m_manager_alive(manager.lifetime_token())
	, m_layer(&layer)
	, m_name(std::move(name))
	, m_id(id)
{
}

scn::schema_component_registration::~schema_component_registration()
{
	reset();
}

scn::schema_component_registration::schema_component_registration(schema_component_registration&& other) noexcept
	: m_manager(std::exchange(other.m_manager, nullptr))
	, m_manager_alive(std::move(other.m_manager_alive))
	, m_layer(std::exchange(other.m_layer, nullptr))
	, m_name(std::move(other.m_name))
	, m_id(std::exchange(other.m_id, 0))
{
}

scn::schema_component_registration& scn::schema_component_registration::operator=(schema_component_registration&& other) noexcept
{
	if (this != &other) {
		reset();
		m_manager = std::exchange(other.m_manager, nullptr);
		m_manager_alive = std::move(other.m_manager_alive);
		m_layer = std::exchange(other.m_layer, nullptr);
		m_name = std::move(other.m_name);
		m_id = std::exchange(other.m_id, 0);
	}
	return *this;
}

void scn::schema_component_registration::reset()
{
	scene_layer* layer = std::exchange(m_layer, nullptr);
	if (!layer) {
		return;
	}
	// Blobs laid out by this schema must not outlive it: a schema of the same name registered again may lay them out
	// otherwise. Only the worlds of this layer: a sibling layer (the next one of a hot reload) has its own registration
	// of the name, and its worlds hold blobs of that one.
	if (!m_manager_alive.expired()) {
		m_manager->clear_component_pools(m_id, *layer);
	}
	layer->assembler().unregister_desc_spawner(m_name);
	layer->catalog().remove(m_name);
	layer->components().remove(m_name);
	layer->descs().unregister_desc(m_name);
	m_manager = nullptr;
	m_manager_alive.reset();
}

scn::schema_component_registration scn::register_schema_component(context_manager& manager, scene_layer& layer, desc::type_schema schema)
{
	const std::string name = schema.name;
	const auto refuse = [&name](std::string_view why) {
		egLOG_ERROR("scene", "component without a C++ type '{}' not registered: {}", name, why);
		return schema_component_registration{};
	};
	if (name.empty()) {
		return refuse("the schema has no name");
	}
	if (!schema.is_component) {
		return refuse("the schema does not describe a component");
	}
	const entt::id_type id = schema_component_id(name);
	if (layer.components().find(name) || layer.catalog().find(name)) {
		return refuse("the name is taken");
	}
	if (const std::string* other = layer.components().name_of(id)) {
		return refuse(std::string{ "its id is taken by '" } + *other + "'");
	}
	if (const component_entry* other = layer.catalog().find(id)) {
		return refuse(std::string{ "its id is taken by '" } + other->name + "'");
	}
	if (layer.descs().is_registered(name) || layer.descs().find_schema(name)) {
		return refuse("a desc type of that name exists");
	}
	if (layer.assembler().has_spawner(name)) {
		return refuse("a spawner of that name exists");
	}
	// desc_system::register_schema only asserts this; a release build would register the wrong version.
	if (const int current = layer.descs().current_version(name); schema.version != current) {
		return refuse(std::format("the schema is version {}, its migrations lead to version {}", schema.version, current));
	}

	// Every step below may have run or thrown half-way; the name was free in the layer, so all of it is ours to undo.
	const auto undo = [&] {
		layer.assembler().unregister_desc_spawner(name);
		layer.catalog().remove(name);
		layer.components().remove(name);
		layer.descs().unregister_desc(name);
	};
	try {
		// A schema built in code rather than parsed has no layout yet, and bind_runtime throws without one.
		if (schema.layout.size == 0 && !schema.fields.empty()) {
			desc::compute_layout(schema, &layer.descs().schemas());
		}
		const desc::type_schema* stored = layer.descs().register_schema(std::move(schema));
		if (!stored || !layer.descs().is_registered(name)) {
			undo();
			return refuse("its schema cannot be bound (a nested or enum schema it names is missing?)");
		}
		std::shared_ptr<const desc::schema_binding> binding = desc::bind_runtime(*stored, layer.descs().schemas());
		if (!binding) {
			undo();
			return refuse("its schema cannot be bound");
		}
		if (const std::string field = unsupported_array(*binding); !field.empty()) {
			undo();
			return refuse(std::format("its field '{}' is an array of structures or custom values, not supported yet", field));
		}
		if (!layer.components().add(name, id)) {
			undo();
			return refuse("the component registry refused it");
		}
		if (!layer.catalog().add_schema_component(name, binding)) {
			undo();
			return refuse("the catalog refused it");
		}
		const auto spawner = [id, binding](entt::registry& reg, entt::entity entity, const desc::desc_base& source, std::string_view) {
			spawn(id, *binding, reg, entity, source);
		};
		const auto patcher = [id, binding](entt::registry& reg, entt::entity entity, const desc::desc_base& source) {
			spawn(id, *binding, reg, entity, source);
		};
		if (!layer.assembler().register_desc_spawner(name, spawner, patcher)) {
			undo();
			return refuse("the assembler refused its spawner");
		}
	} catch (const std::exception& error) {
		undo();
		return refuse(error.what());
	}
	return schema_component_registration{ manager, layer, name, id };
}
