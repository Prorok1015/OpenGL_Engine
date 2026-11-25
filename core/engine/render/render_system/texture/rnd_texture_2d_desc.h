#pragma once
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/resource/resources/res_resource_picture.h>
#include <snk/resource/res_image_format.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rnd
{
	// The pixels a desc holds itself (#1204): rows one after another without padding, top row first.
	struct inline_pixels
	{
		uint32_t width = 0;
		uint32_t height = 0;
		driver::texture_header::TYPE format = driver::texture_header::TYPE::RGBA8;
		std::vector<unsigned char> bytes;
	};

	class texture_2d_desc : public texture_desc
	{
	public:
		// Inline pixels are for small textures (a colour, a checkerboard); a bigger one is a picture file.
		static constexpr std::size_t k_max_inline_bytes = res::image_format::max_inline_pixel_bytes;
		static constexpr uint32_t k_max_inline_side = res::image_format::max_inline_pixel_side;

		using base_type = texture_desc;
		texture_2d_desc() = default;
		virtual ~texture_2d_desc() override = default;
		texture_2d_desc(const texture_2d_desc&) = default;
		texture_2d_desc(texture_2d_desc&&) noexcept = default;
		texture_2d_desc& operator=(const texture_2d_desc&) = default;
		texture_2d_desc& operator=(texture_2d_desc&&) noexcept = default;
		virtual void copy_to(desc::desc_base& other) const override
		{
			copy_to_same(*this, other);
		}
		virtual void deserialize(desc::desc_system& system, const json::object& resource) override;
		virtual void serialize(json::object& resource) const override;

		res::res_handle<res::picture_resource> get_picture_handle() const { return picture_handle; }
		// Set when the desc holds its pixels (`pixels`) instead of naming a picture (`data`).
		const std::optional<inline_pixels>& get_inline_pixels() const { return m_pixels; }

	private:
		res::res_handle<res::picture_resource> picture_handle;
		std::optional<inline_pixels> m_pixels;
	};
}