#pragma once
#include "edt_editor_events.h"
#include <snk/ecs/ecs_entity.h>
#include <entt/fwd.hpp>
#include <functional>
#include <string>

namespace edt
{
	// Owns the current selection and announces every change through editor_events::selection_changed.
	// The selection is a node-id path, so it survives the desc being rewritten; the entity behind it is
	// looked up on demand. Any edit that rewrites the desc clears it first (scene_editor::on_before_modify),
	// as does a change of the active world.
	class selection_service
	{
	public:
		using registry_provider = std::function<entt::registry*(const std::string& world)>;

		explicit selection_service(editor_events& events);
		selection_service(const selection_service&) = delete;
		selection_service& operator=(const selection_service&) = delete;

		// Always emits, even when the new value equals the old one: the panels re-read their node on it.
		void select(selection value);
		void clear();

		const selection& current() const { return m_current; }
		bool has_selection() const { return !m_current.empty(); }

		// Where the world's entities live; without one entity() is always null.
		void set_registry_provider(registry_provider provider) { m_registry_of = std::move(provider); }
		// The entity at the selected path in the selected world; entt::null when nothing is selected or it is not spawned.
		ecs::entity entity() const;

	private:
		editor_events& m_events;
		selection m_current;
		registry_provider m_registry_of;
		connection m_world_changed;
	};
}
