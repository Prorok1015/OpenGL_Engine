#pragma once
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>

namespace desc { class desc_system; }

namespace rnd
{
	// Registered on the root resource system: a layer's require<geometry_desc> is then answered by the root, so a world
	// and the renderer share one block and the file is read once (#1021). A geometry_desc subtype defined in a game
	// module is not created here.
	class geometry_desc_adapter : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = ::res::adapter_info::make<rnd::geometry_desc>({ "desc" });

		geometry_desc_adapter(::res::resource_system& res_system, ::desc::desc_system& desc_system);

		std::vector<::res::res_dep> collect_dependencies(const ::res::tag& tag,
			const std::vector<std::byte>& raw_data) const override;
		using core::res::adapter_interface::deserialize;
		// A desc of another type fails the load: it is not handed out as a geometry.
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data,
			const ::res::res_deps& deps) const override;
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag,
			const std::vector<std::byte>& raw_data) const override;
		std::vector<std::byte> serialize(const ::res::tag& tag,
			const std::shared_ptr<::res::resource_entry>& resource) const override;

	private:
		core::desc::desc_adapter_t m_descs;
	};
}
