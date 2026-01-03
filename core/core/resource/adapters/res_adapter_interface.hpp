#pragma once
#include "res_tag.h"
#include "resources/res_resource_base.h"
#include "adapters/res_adapter_info.hpp"
#include <memory>
#include <vector>

namespace core::res
{
	class adapter_interface
	{
	public:
		virtual ~adapter_interface() = default;
		virtual std::shared_ptr<::res::resource_entry> deserialize(const ::res::tag& tag, const std::vector<std::byte>& raw_data) const = 0;
		virtual std::vector<std::byte> serialize(const ::res::tag& tag, const std::shared_ptr<::res::resource_entry>& resource) const = 0;
	};
}