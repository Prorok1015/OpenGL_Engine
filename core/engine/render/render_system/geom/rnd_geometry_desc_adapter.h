#pragma once
#include <snk/desc/adapters/desc_adapter_t.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>

namespace desc { class desc_system; }

namespace rnd
{
	// On the root, so a layer's require<geometry_desc> gets the block the renderer gets (#1021). A geometry_desc
	// subtype defined in a game module is not created here. Reads the ranges of the binary file next to the desc
	// (EPIC-79, #1068) and checks the data before it reaches the GPU.
	class geometry_desc_adapter : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = ::res::adapter_info::make<rnd::geometry_desc>({ "desc" });

		geometry_desc_adapter(::res::resource_system& res_system, ::desc::desc_system& desc_system);

		// The parent, and the binary file when "vertices" or "indices" is a range: a change of it reloads the geometry.
		std::vector<::res::res_dep> collect_dependencies(const ::res::tag& tag,
			const std::vector<std::byte>& raw_data) const override;
		using core::res::adapter_interface::deserialize;
		// A desc of another type fails the load: it is not handed out as a geometry.
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data,
			const ::res::res_deps& deps) const override;
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag,
			const std::vector<std::byte>& raw_data) const override;
		// Throws std::runtime_error: a geometry is written with its binary file by rnd::write_geometry_files (EPIC-79).
		std::vector<std::byte> serialize(const ::res::tag& tag,
			const std::shared_ptr<::res::resource_entry>& resource) const override;

	private:
		// The ranges deserialize recorded: read, checked, copied into the vectors. Throws res::load_error.
		void load_binary(const ::res::tag& tag, rnd::geometry_desc& geometry) const;

		::res::resource_system& m_resources;
		core::desc::desc_adapter_t m_descs;
	};
}
