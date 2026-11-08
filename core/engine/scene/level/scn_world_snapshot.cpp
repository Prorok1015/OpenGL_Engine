#include <snk/scene/level/scn_world_snapshot.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/scene/level/scn_world_context.h>
#include <snk/scene/scn_component_catalog.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_time.h>
#include <snk/common/logger/engine_log.h>
#include <snk/ecs/ecs_event.hpp>
#include <snk/ecs/ecs_random.hpp>
#include <boost/json/array.hpp>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
	constexpr std::string_view k_type = "world_snapshot";
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

	bool is_editor_only(const entt::registry& reg, entt::entity entity, const std::vector<const scn::component_entry*>& editor_only)
	{
		for (const auto* entry : editor_only) {
			if (entry->contains(reg, entity)) {
				return true;
			}
		}
		return false;
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

json::object scn::capture_world(const world_context& world, const component_catalog& catalog, snapshot_report& report)
{
	const entt::registry& reg = world.state();

	std::vector<const component_entry*> data;
	std::vector<const component_entry*> ctx;
	std::vector<const component_entry*> editor_only;
	catalog.for_each([&](const component_entry& entry) {
		switch (entry.category) {
		case component_category::data: data.push_back(&entry); break;
		case component_category::ctx: ctx.push_back(&entry); break;
		case component_category::editor_only: editor_only.push_back(&entry); break;
		case component_category::derived: break;
		}
	});

	json::array entities;
	if (const auto* storage = reg.storage<entt::entity>()) {
		for (const entt::entity entity : *storage) {
			if (!reg.valid(entity) || is_editor_only(reg, entity, editor_only)) {
				continue;
			}
			json::object components;
			for (const auto* entry : data) {
				if (!entry->contains(reg, entity)) {
					continue;
				}
				if (!entry->saved()) {
					++report.unsaved[entry->name];
					continue;
				}
				json::object value;
				entry->write(*entry->schema, reg, entity, value);
				components[entry->name] = std::move(value);
			}
			json::object item;
			item["snap"] = static_cast<std::uint64_t>(entt::to_integral(entity));
			item["components"] = std::move(components);
			entities.push_back(std::move(item));
		}
	}
	report.entities += entities.size();

	for (const auto& missing : unregistered_storages(reg, catalog)) {
		report.unsaved[missing.type] += reg.storage(missing.id)->size();
	}

	json::object resources;
	for (const auto* entry : ctx) {
		if (!entry->contains(reg, entt::null)) {
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

bool scn::restore_world(world_context& world, const json::object& snapshot, const component_catalog& catalog,
	const desc::read_context& context, snapshot_report& report)
{
	const json::value* type = snapshot.if_contains("__type");
	const json::value* entities_value = snapshot.if_contains("entities");
	if (!type || !type->is_string() || type->as_string() != k_type || !entities_value || !entities_value->is_array()) {
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
	desc::read_context read = context;
	read.entities = &ids;

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
				if (!entry.read(*entry.schema, reg, created[i], value->as_object(), read)) {
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
			if (!entry->read(*entry->schema, reg, entt::null, value.as_object(), read)) {
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
