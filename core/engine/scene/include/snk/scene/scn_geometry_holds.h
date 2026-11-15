#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <cstddef>
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
		static geometry_holds& of(entt::registry& reg) { return attach(reg, nullptr); }

		void hold(entt::entity entity, res::res_handle<rnd::geometry_desc> geometry);
		// The tag through the resources, unless reading it is refused (res::reference_problem, as the renderer
		// refuses it).
		void hold(entt::entity entity, const res::tag& geometry);
		void release(entt::entity entity);
		// Valid until the next hold or release.
		const res::res_handle<rnd::geometry_desc>* find(entt::entity entity) const;
		std::size_t size() const;

	private:
		res::resource_system* m_resources = nullptr;
		std::unordered_map<entt::entity, res::res_handle<rnd::geometry_desc>> m_held;
	};

}
