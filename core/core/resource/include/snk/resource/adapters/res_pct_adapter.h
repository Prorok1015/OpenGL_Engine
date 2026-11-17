#pragma once
#include <cstddef>
#include <snk/resource/adapters/res_adapter_interface.hpp>
#include <snk/resource/adapters/res_adapter_info.hpp>
#include <snk/resource/resources/res_resource_picture.h>
#include <snk/resource/res_image_format.h>
#include <snk/common/snk_api.h>
#include <string_view>
#include <optional>

namespace res
{
	class SNK_API raw_image_adapter : public core::res::adapter_interface
	{
	public:
		struct raw_image_header
		{
			glm::ivec2 size;
			std::int32_t channels;
		};

		static constexpr auto HEADER_SIZE = sizeof(raw_image_header);
		static constexpr auto EXTENSION = image_format::raw_extension;
		static inline auto INFO = res::adapter_info::make<res::picture_resource>({std::string{ "raw" }});

		std::optional<raw_image_header> read_header(const std::vector<std::byte>& data) const;
		std::shared_ptr<res::picture_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;

		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}

		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};
	static_assert(raw_image_adapter::HEADER_SIZE == image_format::raw_header_size,
		"image_format::raw_header is the header raw_image_adapter reads");

	class SNK_API pct_adapter : public core::res::adapter_interface
	{
	public:
		static constexpr auto EXTENSIONS = image_format::picture_extensions;
		static inline auto INFO = res::adapter_info::make<res::picture_resource>({std::string{ "pct" }, std::string{ "png" }, std::string{ "jpeg" }, std::string{ "jpg" }} );
		// What one picture may decode to: no side longer than a GPU takes, and no more pixel bytes than this.
		static constexpr int max_side = 16384;
		static constexpr std::size_t max_decoded_bytes = std::size_t{ 512 } * 1024 * 1024;

		pct_adapter();

		// The memory stb_image::Image lets its decoder hold for a picture of this header and file size (decode_budget).
		static std::size_t decode_budget_bytes(int width, int height, int channels, std::size_t encoded_bytes);
		// The most the decoder held at once in this thread's last decode, refused or not.
		static std::size_t last_decode_peak_bytes();

		std::shared_ptr<res::picture_resource> operator()(res::tag tag, const std::vector<std::byte>& data) const;
		virtual std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw_data) const override
		{
			return operator()(tag, raw_data);
		}

		virtual std::vector<std::byte> serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const override;
	};
}