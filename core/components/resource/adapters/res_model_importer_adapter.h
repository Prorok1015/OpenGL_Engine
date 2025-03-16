#pragma once
#include "common.h"
#include "res_resource_base.h"
#include <memory>


namespace res
{
	class model_importer_adapter
	{
	public:
		std::shared_ptr<res::Resource> operator()(const res::tag& tag, const std::vector<std::byte>& data) const;

	};
}