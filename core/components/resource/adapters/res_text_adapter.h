#pragma once
#include "res_resource_text_file.h"
#include "adapters/res_adapter_info.hpp"

namespace res
{
	class text_adapter
	{
	public:
		static constexpr auto EXTENSIONS = { "txt" };
		static inline auto INFO = res::adapter_info::make<res::TextFile>();
		std::shared_ptr<res::Resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
	};
}