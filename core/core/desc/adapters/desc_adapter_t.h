#pragma once
#include "adapters/res_adapter_interface.hpp"
#include "res_system.h"
#include "res_tag.h"
#include "desc_base.hpp"

namespace desc {
	class desc_system;
}

namespace core::desc
{
	class desc_adapter_t : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = ::res::adapter_info::make<::desc::desc_base>({ "desc" });

		desc_adapter_t(::res::resource_system& res_system, ::desc::desc_system& desc_system);

		virtual std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const;
		virtual std::vector<std::byte> serialize(const ::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const;
	private:
		::res::resource_system& m_res_system;
		::desc::desc_system& m_desc_system;
	};
}