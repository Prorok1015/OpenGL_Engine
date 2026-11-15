#pragma once
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/res_system.h>
#include <snk/common/snk_api.h>
#include <entt/entity/entity.hpp>
#include <entt/fwd.hpp>
#include <cstddef>
#include <unordered_map>

namespace scn {

	// The geometry each mesh entity draws, held from its assembly on (#1021): geometry_component keeps only the tag,
	// and the resource cache lets go of a resource nobody holds, so the first frame that draws it parsed it again.
	// Engine-only, outside the module layout key: a registry's ctx, made and filled by assemble_mesh_node.
	class SNK_API geometry_holds
	{
	public:
		// The holds of `reg`, made on first use; its entities let go of theirs when their geometry_component is
		// removed, replaced by another tag, or destroyed with the registry.
		static geometry_holds& of(entt::registry& reg);

		void hold(entt::entity entity, res::res_handle<rnd::geometry_desc> geometry);
		void release(entt::entity entity);
		const res::res_handle<rnd::geometry_desc>* find(entt::entity entity) const;
		std::size_t size() const { return m_held.size(); }

	private:
		std::unordered_map<entt::entity, res::res_handle<rnd::geometry_desc>> m_held;
	};

}
