#pragma once
#include "adapters/res_adapter_interface.hpp"
#include "res_system.h"
#include "res_tag.h"
#include "desc_base.hpp"
#include "desc_system.h"
#include "level/scn_world_desc.h"

namespace scn {
	class worldwrap_adapter : public core::res::adapter_interface {
	public:
		static inline auto INFO = ::res::adapter_info::make<scn::world_desc>({ "desc" });

		worldwrap_adapter(res::resource_system& res_system, desc::desc_system& desc_system);
		~worldwrap_adapter() = default;

		virtual std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const;
		virtual std::vector<std::byte> serialize(const ::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const;
	private:
		::res::resource_system& m_res_system;
		::desc::desc_system& m_desc_system;
	};
}