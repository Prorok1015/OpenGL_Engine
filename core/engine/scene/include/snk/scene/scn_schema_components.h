#pragma once
#include <snk/common/snk_api.h>
#include <snk/desc/desc_schema.h>
#include <snk/desc/desc_schema_ops.h>
#include <entt/core/fwd.hpp>
#include <entt/entity/fwd.hpp>
#include <memory>
#include <string>
#include <string_view>

// Components without a C++ type (#606, DESC_SCHEMA.md section 3, owner decision 5): a component described only by a
// desc::type_schema lives in its world as a desc::dyn_blob, in the pool reg.storage<desc::dyn_blob>(id) of its own id.
// The blob is laid out as the schema's dynamic_desc lays it out, so spawning one copies the desc's blob.
namespace scn
{
	class context_manager;
	class scene_layer;
	struct component_entry;

	// The storage id of a component without a C++ type: entt::hashed_string of its stable name. Never saved (snapshots
	// and files name the component); a collision with another component is refused when it is registered.
	SNK_API entt::id_type schema_component_id(std::string_view name);

	// The blob `entity` holds in the pool `id`; null when it holds none.
	SNK_API desc::dyn_blob* find_schema_component(entt::registry& reg, entt::entity entity, entt::id_type id);
	SNK_API const desc::dyn_blob* find_schema_component(const entt::registry& reg, entt::entity entity, entt::id_type id);
	// Places a copy of `blob` as the component `id` of `entity`, replacing the one it has. The copy is the engine's, its
	// handles in the default resource: a blob never crosses into the engine by value, so what a module built (with its own
	// Boost.JSON on MSVC) is not what the world keeps (#785).
	SNK_API desc::dyn_blob& emplace_schema_component(entt::registry& reg, entt::entity entity, entt::id_type id, const desc::dyn_blob& blob);

	// Whether the entry is a component without a C++ type (component_catalog::add_schema_component).
	SNK_API bool is_schema_component(const component_entry& entry);
	// Whether `entity` has the component of `entry`. Use it instead of entry.contains: the entry of a component
	// without a C++ type cannot answer through that function, which is not handed the component's id.
	SNK_API bool holds(const component_entry& entry, const entt::registry& reg, entt::entity entity);

	// Undoes a register_schema_component: the component's pools are cleared in every world of every set of its layer
	// (a sibling layer registers the name on its own and keeps its worlds), then its spawner, catalog entry, name and
	// desc type go. Keep it in the layer (scene_layer::keep): it must not
	// outlive the layer, and the layer drops it after its own worlds are gone.
	class SNK_API schema_component_registration
	{
	public:
		schema_component_registration() = default;
		// What register_schema_component returns once everything is registered.
		schema_component_registration(context_manager& manager, scene_layer& layer, std::string name, entt::id_type id);
		~schema_component_registration();
		schema_component_registration(schema_component_registration&& other) noexcept;
		schema_component_registration& operator=(schema_component_registration&& other) noexcept;
		schema_component_registration(const schema_component_registration&) = delete;
		schema_component_registration& operator=(const schema_component_registration&) = delete;

		explicit operator bool() const { return m_layer != nullptr; }
		const std::string& name() const { return m_name; }
		entt::id_type id() const { return m_id; }
		void reset();

	private:
		context_manager* m_manager = nullptr;
		std::weak_ptr<const bool> m_manager_alive;
		scene_layer* m_layer = nullptr;
		std::string m_name;
		entt::id_type m_id = 0;
	};

	// Registers a component described only by `schema` (a component schema, `is_component`) in `layer`: its desc type
	// (dynamic_desc), a spawner that copies the desc's blob into the world, its name and id (components()) and a catalog
	// entry, so snapshots, hot reload and runtime.get see it. Empty (logged, nothing registered) when the schema is no
	// component, cannot be bound, is not the version its migrations lead to, or its name, desc type or id is taken in
	// the layer or below. A schema built in code gets its layout here. The spawner refuses a desc read by another
	// layout of the schema (one cached from an earlier registration of the name).
	SNK_API schema_component_registration register_schema_component(context_manager& manager, scene_layer& layer, desc::type_schema schema);
}
