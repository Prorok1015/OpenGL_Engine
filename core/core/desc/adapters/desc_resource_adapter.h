#pragma once
#include "common.h"
#include "resources/res_resource_base.h"
#include "resources/desc_resource.h"
#include "adapters/res_adapter_info.hpp"
#include "adapters/res_adapter_interface.hpp"

namespace desc
{
	struct desc_adapter : public core::res::adapter_interface
	{
		static constexpr auto EXTENSION = "desc"sv;
		static inline auto INFO = res::adapter_info::make<desc::desc_resource>({ "desc"s });

		std::shared_ptr<desc::desc_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}
		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};
}
