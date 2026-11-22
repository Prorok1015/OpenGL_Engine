#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_blackboard_catalog.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_schema_components.h>
#include <snk/scene/scn_time.h>
#include <snk/common/logger/engine_log.h>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_event.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_schema_ops.h>
#include <snk/desc/desc_system.h>
#include <algorithm>
#include <string>
#include <boost/json/array.hpp>
#include <cstdint>
#include <format>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	constexpr std::string_view k_type = "world_snapshot";
	// The format capture_world writes. A change of it raises this with a migration step registered for k_type.
	constexpr std::int64_t k_version = 1;
	// Restored in the first pass with the entity: node_index must know every path before any other component lands.
	constexpr std::string_view k_node_id = "scn.node_id";

	class snapshot_ids final : public desc::entity_remap
	{
	public:
		void add(std::uint32_t saved, entt::entity restored) { m_ids[saved] = static_cast<std::uint32_t>(entt::to_integral(restored)); }

		std::optional<std::uint32_t> find(std::uint32_t saved) const override
		{
			const auto it = m_ids.find(saved);
			if (it == m_ids.end()) {
				return std::nullopt;
			}
			return it->second;
		}

	private:
		std::unordered_map<std::uint32_t, std::uint32_t> m_ids;
	};

	// Desc references are read with an owner desc (desc_ref_traits): its tag names the memory copies of inline descs.
	// A component has no owner, so the restore stands one in, named after the world.
	class snapshot_owner final : public desc::desc_base
	{
	public:
		explicit snapshot_owner(std::string_view world)
		{
			std::string tag{ "memory://snapshot/" };
			tag.append(world);
			set_tag(res::tag{ std::move(tag) });
		}

		void deserialize(desc::desc_system&, const json::object&) override {}
		void serialize(json::object&) const override {}
	};

	// Set by capture_world with a desc system: the version of each saved type's steps, by catalog name (#1233).
	constexpr std::string_view k_component_versions = "component_versions";

	// What restore_world reads of one saved value: the value itself, the value brought through the steps of its type
	// from the version it was captured at (into `storage`), or nullptr when it cannot be read (logged).
	const json::object* current_format(const scn::component_entry& entry, const json::object& saved, const json::object* versions,
		const desc::desc_system* descs, std::string_view world, json::object& storage)
	{
		const json::value* recorded = versions ? versions->if_contains(entry.name) : nullptr;
		if (!recorded || !descs || !recorded->is_int64()) {
			return &saved;
		}
		const std::string& type = entry.schema->schema->name;
		const int current = descs->current_version(type);
		const std::int64_t version = recorded->as_int64();
		if (version == current) {
			return &saved;
		}
		if (version < 1 || version > current) {
			egLOG_WARN("scene", "world '{}': '{}' was captured at version {}, but its type '{}' is version {} now; not restored",
				world, entry.name, version, type, current);
			return nullptr;
		}
		storage = saved;
		storage["__type"] = type;
		storage["__version"] = version;
		try {
			descs->migrate_tree(storage, std::format("world '{}' {}", world, entry.name));
		} catch (const std::exception& error) {
			egLOG_WARN("scene", "world '{}': '{}' not restored: {}", world, entry.name, error.what());
			return nullptr;
		}
		storage.erase("__type");
		storage.erase("__version");
		return &storage;
	}

	std::optional<std::uint64_t> to_u64(const json::value* value)
	{
		std::uint64_t number = 0;
		if (!value || !desc::json_to_uint64(*value, number)) {
			return std::nullopt;
		}
		return number;
	}

	template<typename Event>
	void raise(entt::registry& reg, entt::entity entity)
	{
		if (auto* event = reg.ctx().find<ecs::event<Event>>()) {
			event->emit(entity);
		} else {
			reg.ctx().emplace<ecs::event<Event>>().emit(entity);
		}
	}

	// The engine's derived components, from the data they follow; the transform phase of the first frame fills them.
	void rebuild_derived(entt::registry& reg)
	{
		for (const auto entity : reg.view<scn::local_transform>()) {
			reg.emplace_or_replace<scn::world_transform>(entity);
			raise<scn::transform_updated>(reg, entity);
			raise<scn::hierarchy_updated>(reg, entity);
		}
		for (const auto [entity, skeleton] : reg.view<scn::skeleton_component>().each()) {
			if (skeleton.bone_count > 0) {
				reg.emplace_or_replace<scn::bone_matrices_component>(entity).matrices.assign(static_cast<std::size_t>(skeleton.bone_count), glm::mat4{ 1.0f });
			}
		}
	}
}

scn::entity_writer::entity_writer(const component_catalog& catalog)
{
	catalog.for_each([this](const component_entry& entry) {
		if (entry.category == component_category::data) {
			m_data.push_back(&entry);
		} else if (entry.category == component_category::editor_only) {
			m_editor_only.push_back(&entry);
		}
	});
}

bool scn::entity_writer::editor_only(const entt::registry& reg, entt::entity entity) const
{
	for (const auto* entry : m_editor_only) {
		if (scn::holds(*entry, reg, entity)) {
			return true;
		}
	}
	return false;
}

std::vector<std::string> scn::entity_writer::present(const entt::registry& reg, entt::entity entity) const
{
	std::vector<std::string> names;
	for (const auto* entry : m_data) {
		if (scn::holds(*entry, reg, entity)) {
			names.push_back(entry->name);
		}
	}
	return names;
}

scn::entity_writer::components scn::entity_writer::write(const entt::registry& reg, entt::entity entity, const std::vector<std::string>* only) const
{
	components out;
	for (const auto* entry : m_data) {
		if (only && std::ranges::find(*only, entry->name) == only->end()) {
			continue;
		}
		if (!scn::holds(*entry, reg, entity)) {
			continue;
		}
		if (!entry->saved()) {
			out.unsaved.push_back(entry->name);
			continue;
		}
		json::object value;
		// Not written (a blob of another layout): saved as {} it would come back as the defaults, so it is unsaved.
		if (!entry->write(*entry->schema, reg, entity, value)) {
			out.unsaved.push_back(entry->name);
			continue;
		}
		out.saved[entry->name] = std::move(value);
	}
	return out;
}

namespace
{
	// Set by capture_world with a desc system: every typed object inside carries its version.
	constexpr std::string_view k_typed_versions = "typed_versions";
}

json::object scn::capture_world(const world_context& world, const component_catalog& catalog, const desc::desc_system& descs,
	snapshot_report& report)
{
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	json::object out = capture_world(world, catalog, report);
#if defined(_MSC_VER)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
	descs.stamp_tree(out["ctx"]);
	descs.stamp_tree(out["entities"]);
	out[k_typed_versions] = true;
	json::object versions;
	const auto note_version = [&](std::string_view name) {
		const component_entry* entry = catalog.find(name);
		if (entry && entry->saved() && !versions.contains(name)) {
			versions[name] = descs.current_version(entry->schema->schema->name);
		}
	};
	if (const json::array* entities = out["entities"].if_array()) {
		for (const json::value& entity : *entities) {
			const json::value* components = entity.is_object() ? entity.as_object().if_contains("components") : nullptr;
			if (components && components->is_object()) {
				for (const auto& component : components->as_object()) note_version(component.key());
			}
		}
	}
	if (const json::object* resources = out["ctx"].if_object()) {
		for (const auto& resource : *resources) note_version(resource.key());
	}
	out[k_component_versions] = std::move(versions);
	return out;
}

json::object scn::capture_world(const world_context& world, const component_catalog& catalog, snapshot_report& report)
{
	const entt::registry& reg = world.state();

	const entity_writer writer(catalog);
	std::vector<const component_entry*> ctx;
	catalog.for_each([&](const component_entry& entry) {
		if (entry.category == component_category::ctx) {
			ctx.push_back(&entry);
		}
	});

	json::array entities;
	if (const auto* storage = reg.storage<entt::entity>()) {
		for (const entt::entity entity : *storage) {
			if (!reg.valid(entity) || writer.editor_only(reg, entity)) {
				continue;
			}
			entity_writer::components parts = writer.write(reg, entity);
			for (const std::string& name : parts.unsaved) {
				++report.unsaved[name];
			}
			json::object item;
			item["snap"] = static_cast<std::uint64_t>(entt::to_integral(entity));
			item["components"] = std::move(parts.saved);
			entities.push_back(std::move(item));
		}
	}
	report.entities += entities.size();

	for (const auto& missing : unregistered_storages(reg, catalog)) {
		report.unsaved[missing.type] += reg.storage(missing.id)->size();
	}

	json::object resources;
	for (const auto* entry : ctx) {
		if (!scn::holds(*entry, reg, entt::null)) {
			continue;
		}
		if (!entry->saved()) {
			++report.unsaved[entry->name];
			continue;
		}
		json::object value;
		entry->write(*entry->schema, reg, entt::null, value);
		resources[entry->name] = std::move(value);
	}

	json::object out;
	out["__type"] = k_type;
	out["__version"] = k_version;
	out["name"] = world.get_name();
	json::object time;
	time["world_time"] = reg.ctx().get<scn::world_time>().seconds;
	time["fixed_accumulator"] = world.stepper().accumulator();
	out["time"] = std::move(time);
	if (const auto* rng = reg.ctx().find<ecs::rng>()) {
		const auto position = rng->save();
		json::object stream;
		stream["state"] = position.state;
		stream["inc"] = position.inc;
		stream["seed"] = position.seed;
		out["rng"] = std::move(stream);
	}
	out["ctx"] = std::move(resources);
	out["entities"] = std::move(entities);
	return out;
}

bool scn::restore_world(world_context& world, const json::object& given, const component_catalog& catalog,
	const desc::read_context& context, snapshot_report& report)
{
	const json::value* given_type = given.if_contains("__type");
	if (!given_type || !given_type->is_string() || given_type->as_string() != k_type) {
		egLOG_ERROR("scene", "world '{}': not a world snapshot", world.get_name());
		return false;
	}
	// EPIC-58 section 7.2: with the world's desc system a snapshot whose typed objects carry their versions goes through
	// its steps whole, so an older one is brought up and a newer one refused, the root or an object inside alike. One
	// without them (the deprecated capture_world) was written in the current format: its unversioned typed objects take
	// the current versions, and only an older root goes through the steps, as before #766. Without a desc system only
	// the version capture_world writes is read.
	std::optional<json::object> upgraded;
	const json::value* typed_versions = given.if_contains(k_typed_versions);
	const bool stamped = typed_versions && typed_versions->is_bool() && typed_versions->as_bool();
	const json::value* given_version = given.if_contains("__version");
	const bool current_root = !given_version || (given_version->is_int64() && given_version->as_int64() == k_version);
	if (context.system && !stamped) {
		upgraded.emplace(given);
		if (json::value* ctx = upgraded->if_contains("ctx")) context.system->stamp_unversioned(*ctx);
		if (json::value* entities = upgraded->if_contains("entities")) context.system->stamp_unversioned(*entities);
	}
	if (context.system && (stamped || !current_root)) {
		if (!upgraded) upgraded.emplace(given);
		if (const std::string problem = context.system->upgrade_in_memory(*upgraded, std::format("world '{}'", world.get_name())); !problem.empty()) {
			egLOG_ERROR("scene", "{}", problem);
			return false;
		}
	} else if (!context.system && !current_root) {
		egLOG_ERROR("scene", "world '{}': '{}' is version {}, not the version {} this engine reads", world.get_name(), k_type,
			json::serialize(*given_version), k_version);
		return false;
	}
	const json::object& snapshot = upgraded ? *upgraded : given;
	const json::value* versions_value = snapshot.if_contains(k_component_versions);
	const json::object* versions = versions_value ? versions_value->if_object() : nullptr;
	json::object migrated;
	const json::value* entities_value = snapshot.if_contains("entities");
	if (!entities_value || !entities_value->is_array()) {
		egLOG_ERROR("scene", "world '{}': not a world snapshot", world.get_name());
		return false;
	}
	entt::registry& reg = world.state();
	if (const auto* storage = std::as_const(reg).storage<entt::entity>(); storage && storage->free_list() != 0) {
		egLOG_ERROR("scene", "world '{}': a snapshot is restored only into an empty world", world.get_name());
		return false;
	}
	const json::array& entities = entities_value->as_array();

	snapshot_ids ids;
	snapshot_owner owner{ world.get_name() };
	desc::read_context read = context;
	read.entities = &ids;
	if (read.system && !read.owner) {
		read.owner = &owner;
	}

	node_index::get(reg);
	const component_entry* node_id = catalog.find(k_node_id);
	std::vector<entt::entity> created;
	created.reserve(entities.size());
	for (const json::value& item : entities) {
		const json::object* object = item.if_object();
		const auto snap = object ? to_u64(object->if_contains("snap")) : std::nullopt;
		if (!snap || *snap > UINT32_MAX) {
			egLOG_ERROR("scene", "world '{}': an entity of the snapshot has no id", world.get_name());
			return false;
		}
		const entt::entity entity = reg.create();
		ids.add(static_cast<std::uint32_t>(*snap), entity);
		created.push_back(entity);
	}
	for (std::size_t i = 0; i < entities.size(); ++i) {
		const json::object* components = entities[i].as_object().if_contains("components") ? entities[i].as_object().at("components").if_object() : nullptr;
		if (node_id && node_id->saved() && components) {
			if (const auto* path = components->if_contains(node_id->name); path && path->is_object()) {
				if (!node_id->read(*node_id->schema, reg, created[i], path->as_object(), read)) {
					++report.partial[node_id->name];
				}
			}
		}
	}

	// The keys each type reads, once per type rather than once per entity.
	std::unordered_map<const component_entry*, std::vector<std::string>> known_by_type;
	for (std::size_t i = 0; i < entities.size(); ++i) {
		const json::value* components_value = entities[i].as_object().if_contains("components");
		if (!components_value || !components_value->is_object()) {
			continue;
		}
		const json::object& components = components_value->as_object();
		for (const auto& [name, value] : components) {
			const component_entry* entry = catalog.find(name);
			if (!entry || !entry->saved() || !value.is_object()) {
				++report.unknown[std::string{ name }];
			}
		}
		catalog.for_each([&](const component_entry& entry) {
			if (&entry == node_id || entry.category != component_category::data || !entry.saved()) {
				return;
			}
			if (const auto* value = components.if_contains(entry.name); value && value->is_object()) {
				const json::object* readable = current_format(entry, value->as_object(), versions, context.system, world.get_name(), migrated);
				if (!readable) {
					++report.unknown[entry.name];
					return;
				}
				const json::object& saved = *readable;
				auto known = known_by_type.find(&entry);
				if (known == known_by_type.end()) known = known_by_type.emplace(&entry, desc::known_keys(*entry.schema)).first;
				const std::vector<std::string> removed = desc::unknown_keys(known->second, saved);
				// A field the schema dropped is reported as removed, not as a field that no longer fits.
				json::object kept;
				if (!removed.empty()) {
					++report.removed_fields[entry.name];
					kept = saved;
					for (const std::string& key : removed) {
						kept.erase(key);
					}
				}
				if (!entry.read(*entry.schema, reg, created[i], removed.empty() ? saved : kept, read)) {
					++report.partial[entry.name];
				}
			}
		});
	}
	report.entities += entities.size();

	if (const auto* resources = snapshot.if_contains("ctx"); resources && resources->is_object()) {
		for (const auto& [name, value] : resources->as_object()) {
			const component_entry* entry = catalog.find(name);
			if (!entry || entry->category != component_category::ctx || !entry->saved() || !value.is_object()) {
				++report.unknown[std::string{ name }];
				continue;
			}
			const json::object* readable = current_format(*entry, value.as_object(), versions, context.system, world.get_name(), migrated);
			if (!readable) {
				++report.unknown[entry->name];
				continue;
			}
			if (!entry->read(*entry->schema, reg, entt::null, *readable, read)) {
				++report.partial[entry->name];
			}
		}
	}

	if (const auto* time = snapshot.if_contains("time"); time && time->is_object()) {
		double world_time = 0.0;
		double accumulator = 0.0;
		const auto& object = time->as_object();
		if (const auto* seconds = object.if_contains("world_time")) {
			desc::json_to_double(*seconds, world_time);
		}
		if (const auto* rest = object.if_contains("fixed_accumulator")) {
			desc::json_to_double(*rest, accumulator);
		}
		world.restore_time(world_time, accumulator);
	}
	if (const auto* stream = snapshot.if_contains("rng"); stream && stream->is_object()) {
		const auto& object = stream->as_object();
		const auto state = to_u64(object.if_contains("state"));
		const auto inc = to_u64(object.if_contains("inc"));
		const auto seed = to_u64(object.if_contains("seed"));
		if (state && inc && seed) {
			ecs::rng rng;
			rng.load({ *state, *inc, *seed });
			reg.ctx().insert_or_assign(rng);
		}
	}

	rebuild_derived(reg);
	for (const auto& [name, count] : report.unknown) {
		egLOG_WARN("scene", "world '{}': {} of the snapshot is not a saved type any more ({} times), left out", world.get_name(), name, count);
	}
	for (const auto& [name, count] : report.partial) {
		egLOG_WARN("scene", "world '{}': {} restored with a field reset to its default ({} times)", world.get_name(), name, count);
	}
	return true;
}

json::array scn::capture_blackboard(const ecs::blackboard_data& board, const blackboard_catalog& types, snapshot_report& report)
{
	json::array out;
	for (const auto& [key, entry] : board.committed()) {
		const blackboard_type* type = types.find(entry.type);
		if (!type || !entry.value) {
			++report.unsaved[std::string{ entry.type_name }];
			continue;
		}
		json::object value;
		type->write(*type->schema, entry.value.get(), value);
		out.push_back(json::object{ { "key", key }, { "type", type->name }, { "value", std::move(value) } });
	}
	return out;
}

void scn::restore_blackboard(ecs::blackboard_data& board, const json::array& values, const blackboard_catalog& types,
	const desc::read_context& context, snapshot_report& report)
{
	for (const json::value& item : values) {
		const json::object* saved = item.if_object();
		const auto field = [saved](std::string_view name) { return saved ? saved->if_contains(name) : nullptr; };
		const json::value* key_field = field("key");
		const json::value* type_field = field("type");
		const json::value* value_field = field("value");
		const json::string* key = key_field ? key_field->if_string() : nullptr;
		const json::string* name = type_field ? type_field->if_string() : nullptr;
		const json::object* value = value_field ? value_field->if_object() : nullptr;
		if (!key || !name || !value) {
			egLOG_WARN("scene", "restore_blackboard: an entry is not {{key, type, value}}, skipped");
			++report.unknown["<malformed blackboard entry>"];
			continue;
		}
		const blackboard_type* type = types.find(std::string_view{ *name });
		if (!type) {
			++report.unknown[std::string{ *name }];
			continue;
		}
		bool fits = true;
		std::shared_ptr<const void> restored = type->read(*type->schema, *value, context, fits);
		if (!fits) ++report.partial[type->name];
		board.restore(std::string_view{ *key }, type->id, type->type_name, std::move(restored));
	}
}

std::string scn::world_snapshot_version_problem(const desc::desc_system& descs)
{
	const int steps = descs.current_version(k_type);
	if (steps == k_version) return {};
	return std::format("'{}' is written at version {}, but its migration steps lead to version {}: raise the version with its step",
		k_type, k_version, steps);
}
