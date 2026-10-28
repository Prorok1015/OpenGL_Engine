#include <snk/scene/scn_registration_scope.h>
#include <snk/desc/desc_describe.h>
#include <snk/scene/level/scn_level_desc.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/scene/scn_anchor_desc.h>
#include <snk/scene/scn_animated_node_desc.h>
#include <snk/scene/scn_animation_clip_desc.h>
#include <snk/scene/scn_animation_collection_desc.h>
#include <snk/scene/scn_animation_controller_desc.h>
#include <snk/scene/scn_bone_desc.h>
#include <snk/scene/scn_camera_desc.h>
#include <snk/scene/scn_directional_light_desc.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_mesh_node_desc.h>
#include <snk/scene/scn_object_desc.h>
#include <snk/scene/scn_screen_anchor_desc.h>
#include <snk/scene/scn_skin_weights_desc.h>
#include <snk/scene/scn_skinning_desc.h>
#include <snk/scene/scn_skybox_desc.h>

scn::registration_scope::registration_scope(desc::desc_system& desc_sys, ecs_assembler& assembler)
	: m_desc_sys(&desc_sys)
	, m_assembler(&assembler)
{
}

scn::registration_scope::~registration_scope()
{
	release();
}

scn::registration_scope::registration_scope(registration_scope&& other) noexcept
	: m_desc_sys(other.m_desc_sys)
	, m_assembler(other.m_assembler)
	, m_entries(std::move(other.m_entries))
{
	other.m_entries.clear();
}

scn::registration_scope& scn::registration_scope::operator=(registration_scope&& other) noexcept
{
	if (this != &other) {
		release();
		m_desc_sys = other.m_desc_sys;
		m_assembler = other.m_assembler;
		m_entries = std::move(other.m_entries);
		other.m_entries.clear();
	}
	return *this;
}

void scn::registration_scope::spawner(std::string_view type,
									  ecs_assembler::desc_spawner_fn spawn,
									  ecs_assembler::desc_patcher_fn patch,
									  spawner_category category)
{
	m_assembler->register_desc_spawner(type, std::move(spawn), std::move(patch), category);
	m_entries.push_back({ entry_kind::spawner, std::string{ type } });
}

void scn::registration_scope::structural_spawner(std::string_view type, ecs_assembler::desc_structural_fn spawn)
{
	m_assembler->register_structural_spawner(type, std::move(spawn));
	m_entries.push_back({ entry_kind::spawner, std::string{ type } });
}

void scn::registration_scope::release()
{
	for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
		if (it->kind == entry_kind::spawner) {
			m_assembler->unregister_desc_spawner(it->type);
		} else if (it->kind == entry_kind::migration) {
			m_desc_sys->unregister_migrations(it->type);
		} else {
			m_desc_sys->unregister_desc(it->type);
		}
	}
	m_entries.clear();
}

namespace {

// Registers a spawner that forwards to `assemble(reg, e, <T>, name)`.
template<class T, class Fn>
void simple_spawner(scn::registration_scope& scope, std::string_view type, Fn assemble)
{
	scope.spawner(type, [assemble](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view name) {
		assemble(reg, e, static_cast<const T&>(base), name);
	});
}

}

scn::registration_scope scn::register_scene_types(desc::desc_system& desc_sys, ecs_assembler& assembler, rnd::skinning_manager& skinning)
{
	registration_scope scope{ desc_sys, assembler };

	scope.desc<material_desc>("material_desc");
	scope.desc<prefab_desc>("prefab_desc");
	scope.desc<world_desc>(world_desc::__type);
	scope.desc<level_desc>("level_desc");
	scope.desc<scene_anchor_desc>("anchor_desc");
	scope.desc<camera_desc>(camera_desc::__type, true);
	scope.desc<directional_light_desc>(directional_light_desc::__type, true);
	scope.desc<screen_anchor_desc>(screen_anchor_desc::__type, true);
	scope.desc<skybox_desc>(skybox_desc::__type, true);
	scope.desc<mesh_node_desc>(mesh_node_desc::__type);
	scope.desc<object_desc>(object_desc::__type);
	scope.desc<bone_desc>(bone_desc::__type);
	scope.desc<skinning_desc>(skinning_desc::__type);
	scope.desc<skin_weights_desc>(skin_weights_desc::__type);
	scope.desc<animation_controller_desc>(animation_controller_desc::__type);
	scope.desc<animated_node_desc>(animated_node_desc::__type);
	scope.desc<animation_clip_desc>(animation_clip_desc::__type);
	scope.desc<animation_collection_desc>(animation_collection_desc::__type);

	// v2: children are an array of nodes with __id and name (the world is a prefab, so it shares the step).
	scope.migration("prefab_desc", 1, migrate_prefab_v1);
	scope.migration(world_desc::__type, 1, migrate_prefab_v1);
	// v3: the override layer of an explicit nested instance becomes `overrides` records.
	scope.migration("prefab_desc", 2, migrate_prefab_v2);
	scope.migration(world_desc::__type, 2, migrate_prefab_v2);
	scope.migration("level_desc", 1, [](json::object&) {}); // the worlds are typed objects and migrate on their own

	// The assembler that runs it: a prefab spawned in a layer builds its components with the layer's.
	auto prefab_spawn = [](const ecs_assembler& running, entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view name) {
		const auto& prefab = static_cast<const prefab_desc&>(base);
		assemble_prefab(running, reg, e, prefab, name, prefab.get_tag());
	};
	scope.structural_spawner("prefab_desc", prefab_spawn);
	scope.structural_spawner(world_desc::__type, prefab_spawn);

	simple_spawner<scene_anchor_desc>(scope, "anchor_desc", assemble_scene_anchor);
	simple_spawner<camera_desc>(scope, camera_desc::__type, assemble_camera);
	simple_spawner<directional_light_desc>(scope, directional_light_desc::__type, assemble_directional_light);
	simple_spawner<screen_anchor_desc>(scope, screen_anchor_desc::__type, assemble_screen_anchor);
	simple_spawner<skybox_desc>(scope, skybox_desc::__type, assemble_skybox);
	simple_spawner<mesh_node_desc>(scope, mesh_node_desc::__type, assemble_mesh_node);
	simple_spawner<object_desc>(scope, object_desc::__type, assemble_object);
	simple_spawner<bone_desc>(scope, bone_desc::__type, assemble_bone);
	simple_spawner<skinning_desc>(scope, skinning_desc::__type, assemble_skinning);
	simple_spawner<animation_controller_desc>(scope, animation_controller_desc::__type, assemble_animation_controller);
	simple_spawner<animated_node_desc>(scope, animated_node_desc::__type, assemble_animated_node);
	simple_spawner<animation_collection_desc>(scope, animation_collection_desc::__type, assemble_animation_collection);

	scope.spawner(skin_weights_desc::__type, [&skinning](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view name) {
		assemble_skin_weights(skinning, reg, e, static_cast<const skin_weights_desc&>(base), name);
	});

	return scope;
}
