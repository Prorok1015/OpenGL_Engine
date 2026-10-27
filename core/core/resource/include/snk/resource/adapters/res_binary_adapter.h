#pragma once
#include <snk/resource/resources/res_resource_binary.h>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/common/snk_api.h>

namespace res
{
	class SNK_API binary_adapter : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = res::adapter_info::make<res::binary_resource>();

		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override;
		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};
}
