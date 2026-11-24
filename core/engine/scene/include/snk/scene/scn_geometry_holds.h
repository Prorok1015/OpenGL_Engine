#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace scn {

	// The geometry each mesh entity draws, held while its geometry_component names it: the cache lets go of what
	// nobody holds, and the component keeps only the tag (#1021). A registry ctx only the engine makes.
	class SNK_API geometry_holds
	{
	public:
		// Made on first use, with hooks that hold each geometry_component made or retagged and let go of one removed.
		// Without `resources` only what assemble_mesh_node hands it is held; a later call keeps the first resources.
		static geometry_holds& attach(entt::registry& reg, res::resource_system* resources);

		geometry_holds();
		~geometry_holds();
		geometry_holds(const geometry_holds&) = delete;
		geometry_holds& operator=(const geometry_holds&) = delete;
		static geometry_holds& of(entt::registry& reg) { return attach(reg, nullptr); }

		void hold(entt::entity entity, res::res_handle<rnd::geometry_desc> geometry);
		// The tag through the resources, unless reading it is refused (res::reference_problem, as the renderer
		// refuses it).
		void hold(entt::entity entity, const res::tag& geometry);
		void release(entt::entity entity);
		// Valid until the next hold or release.
		const res::res_handle<rnd::geometry_desc>* find(entt::entity entity) const;
		std::size_t size() const;
		// Moves whenever a hold is made, replaced or let go, and when a held geometry was reloaded (the hold then takes the new
		// block, as the renderer does, which may still be loading): what was built from the holds is out of date.
		std::uint64_t revision() const;

	private:
		void watch_tag(const res::tag& tag);
		void unwatch_tag(const res::tag& tag);
		void on_changed(const res::tag& tag);

		res::resource_system* m_resources = nullptr;
		std::unordered_map<entt::entity, res::res_handle<rnd::geometry_desc>> m_held;
		// How many holds name each tag: the resource system is watched once per tag.
		std::unordered_map<res::tag, std::size_t> m_watching;
		std::uint64_t m_revision = 0;
	};

}
