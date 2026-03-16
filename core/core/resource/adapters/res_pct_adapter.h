#pragma once
#include "adapters/res_adapter_interface.hpp"
#include "adapters/res_adapter_info.hpp"
#include "resources/res_resource_picture.h"
#include <string_view>
#include <optional>

namespace res
{
	class raw_image_adapter : public core::res::adapter_interface
	{
	public:
		struct raw_image_header
		{
			glm::ivec2 size;
			std::int32_t channels;
		};

		static constexpr auto HEADER_SIZE = sizeof(raw_image_header);
		static constexpr auto EXTENSION = "raw"sv;
		static inline auto INFO = res::adapter_info::make<res::picture_resource>({"raw"s});

		std::optional<raw_image_header> read_header(const std::vector<std::byte>& data) const;
		std::shared_ptr<res::picture_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;

		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}

		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};

	class pct_adapter : public core::res::adapter_interface
	{
	public:
		static constexpr auto EXTENSIONS = std::array{"pct"sv, "png"sv, "jpeg"sv, "jpg"sv};
		static inline auto INFO = res::adapter_info::make<res::picture_resource>({"pct"s, "png"s, "jpeg"s, "jpg"s} );
		pct_adapter();

		std::shared_ptr<res::picture_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}

		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};
}