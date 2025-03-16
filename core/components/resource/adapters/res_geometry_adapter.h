#pragma once
#include "res_resource_base.h"
#include "res_tag.h"
#include <vector>
#include <memory>

namespace res
{
	class geometry_adapter
	{
	public:
		std::shared_ptr<res::Resource> operator() (res::tag, const std::vector<std::byte>& data) const;
	};

}