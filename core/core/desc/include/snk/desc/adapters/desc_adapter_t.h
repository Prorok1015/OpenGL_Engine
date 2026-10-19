#pragma once
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_base.hpp>
#include <snk/common/snk_api.h>

namespace desc {
	class desc_system;
}

namespace core::desc
{
	class SNK_API desc_adapter_t : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = ::res::adapter_info::make<::desc::desc_base>({ "desc" });

		desc_adapter_t(::res::resource_system& res_system, ::desc::desc_system& desc_system);

		// "__parent" is a dependency: it is loaded first and handed to deserialize.
		std::vector<::res::res_dep> collect_dependencies(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		using core::res::adapter_interface::deserialize;
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data, const ::res::res_deps& deps) const override;
		// A file with a "__parent" fails without the parent in `deps`; it is not looked up here.
		std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		std::vector<std::byte> serialize(const ::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const override;
	private:
		::desc::desc_system& m_desc_system;
	};
}