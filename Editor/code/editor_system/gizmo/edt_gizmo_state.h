#pragma once
#include <cstdint>
#include <functional>
#include <utility>
#include <entt/entt.hpp>
#include "edt_registry_token.h"

// The entity ids the viewport and its gizmo hold, kept apart from ImGui so they can be tested. An id means something only
// in the registry it was taken from: a world that is rebuilt gets a new registry, often at the address of the old one and
// with the same ids (#728, as #404 for the Game view).
namespace edt
{
	// The rotate ring of the transform gizmo: which entity it is about and which ring is being dragged (-1: none).
	struct rotate_gizmo_state
	{
		entt::entity ent = entt::null;
		std::uint64_t token = 0;
		int ring = -1;

		void reset()
		{
			ent = entt::null;
			token = 0;
			ring = -1;
		}

		// Called with the selected entity and the token of its registry every frame the gizmo is drawn: another entity or
		// another registry ends the drag that was in progress.
		void sync(entt::entity selected, std::uint64_t registry)
		{
			if (ent == selected && token == registry) return;
			ent = selected;
			token = registry;
			ring = -1;
		}
	};

	// The entity the viewport shows its gizmo for. The selection's events name it (set); a registry that is not the one seen
	// last time, or one that does not hold the entity any more, takes it from the resolver again.
	class viewport_selection
	{
	public:
		using resolver = std::function<entt::entity()>;

		void set_resolver(resolver find) { m_resolver = std::move(find); }
		void set(entt::entity ent) { m_entity = ent; }
		[[nodiscard]] entt::entity get() const { return m_entity; }

		// Every frame, with the registry on show (null: none). True when the entity was taken anew, so whatever depends on
		// it (the rotate ring) must start over. An entity the resolver gives that the registry does not hold leaves none.
		bool refresh(entt::registry* reg)
		{
			const std::uint64_t token = reg ? registry_token(*reg) : 0;
			const bool replaced = token != m_token;
			m_token = token;
			if (!reg) {
				m_entity = entt::null;
				return replaced;
			}
			if (!replaced && (m_entity == entt::null || reg->valid(m_entity))) return false;
			const entt::entity found = m_resolver ? m_resolver() : entt::null;
			m_entity = (found != entt::null && reg->valid(found)) ? found : entt::null;
			return true;
		}

	private:
		resolver m_resolver;
		std::uint64_t m_token = 0;
		entt::entity m_entity = entt::null;
	};
}
