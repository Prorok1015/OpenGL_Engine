#pragma once
#include "resources/res_resource_text.h"
#include "adapters/res_adapter_info.hpp"

namespace res
{
	class text_adapter
	{
	public:
		static inline auto INFO = res::adapter_info::make<res::text_resource>();
		std::shared_ptr<res::text_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
	};
}