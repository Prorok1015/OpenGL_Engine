#pragma once
#include "common.h"
#include "resources/res_resource_base.h"
#include "resources/desc_resource.h"
#include "adapters/res_adapter_info.hpp"

namespace desc
{
	struct desc_adapter
	{
		static constexpr auto EXTENSION = "desc"sv;
		static inline auto INFO = res::adapter_info::make<desc::desc_resource>({ "desc"s });

		std::shared_ptr<desc::desc_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
	};
}
