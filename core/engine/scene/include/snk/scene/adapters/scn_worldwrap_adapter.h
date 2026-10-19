#pragma once
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/scene/level/scn_world_desc.h>
#include <snk/common/snk_api.h>

namespace scn {
	class SNK_API worldwrap_adapter : public core::res::adapter_interface {
	public:
		static inline auto INFO = ::res::adapter_info::make<scn::world_desc>({ "desc" });

		worldwrap_adapter(res::resource_system& res_system, desc::desc_system& desc_system);
		~worldwrap_adapter() = default;

		// A world_desc file's "__parent" is a dependency: it is loaded first and handed to deserialize.
		std::vector<::res::res_dep> collect_dependencies(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		using core::res::adapter_interface::deserialize;
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data, const ::res::res_deps& deps) const override;
		// A world_desc file with a "__parent" fails without the parent in `deps`; it is not looked up here.
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		std::vector<std::byte> serialize(const ::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const override;
	private:
		::desc::desc_system& m_desc_system;
	};
}