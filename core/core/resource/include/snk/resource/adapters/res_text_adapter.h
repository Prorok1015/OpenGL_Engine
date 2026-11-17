#pragma once
#include <snk/resource/resources/res_resource_text.h>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/common/snk_api.h>

namespace res
{
	class SNK_API text_adapter : public core::res::adapter_interface
	{
	public:
		static inline auto INFO = res::adapter_info::make<res::text_resource>();
		std::shared_ptr<res::text_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;

		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}
		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};
}