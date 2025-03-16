#pragma once
#include "common.h"
#include "res_resource_base.h"

namespace desc
{
	struct desc_adapter
	{
		static constexpr auto EXTENSION = "desc"sv;

		std::shared_ptr<res::Resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
	};
}
