#pragma once
#include "res_picture.h"
#include <string_view>
#include <optional>
#include "adapters/res_adapter_info.hpp"

namespace res
{
	class raw_image_adapter
	{
	public:
		struct raw_image_header
		{
			glm::ivec2 size;
			std::int32_t channels;
		};

		static constexpr auto HEADER_SIZE = sizeof(raw_image_header);
		static constexpr auto EXTENSION = "raw"sv;
		static inline auto INFO = res::adapter_info::make<res::Picture>({"raw"s});

		std::optional<raw_image_header> read_header(const std::vector<std::byte>& data) const;
		std::shared_ptr<res::Resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
	};

	class pct_adapter
	{
	public:
		static constexpr auto EXTENSIONS = std::array{"pct"sv, "png"sv, "jpeg"sv, "jpg"sv};
		static inline auto INFO = res::adapter_info::make<res::Picture>({"pct"s, "png"s, "jpeg"s, "jpg"s} );
		pct_adapter();

		std::shared_ptr<res::Resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
	};
}