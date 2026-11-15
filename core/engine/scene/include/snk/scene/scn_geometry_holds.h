#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <cstddef>
#include <unordered_map>

namespace scn {

	// The geometry each mesh entity draws, held while its geometry_component names it (#1021): the component keeps
	// only the tag, and the resource cache lets go of a resource nobody holds, so the first frame that drew it read it
	// again. Engine-only, outside the module layout key: a registry's ctx that only the engine makes.
	class SNK_API geometry_holds
	{
	public:
		// The holds of `reg`, made on first use with the hooks that keep them: a geometry_component made (an assembly,
		// a snapshot restored, a script) or given another tag is held through `resources`, and one removed or destroyed
		// with its entity or registry is let go. Each world of a context set is attached with its layer's resources
		// when it is made, so the hooks see every component; a registry attached later (or without resources) holds
		// only what assemble_mesh_node hands it. A later call keeps the resources it was first given.
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
