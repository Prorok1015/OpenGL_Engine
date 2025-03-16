#pragma once
#include <common.h>
#include "res_tag.h"
#include <rnd_texture_interface.h>
#include <rnd_driver_interface.h>

namespace rnd
{
	class Texture
	{
	public:
		static std::unique_ptr<driver::texture_interface> load(driver::driver_interface* drv, const res::tag& tag);

	public:
		Texture(driver::texture_interface* texture, int width, int height) 
			: texture_impl(texture), width_(width), height_(height) {
		}
		~Texture() {}

		void bind(unsigned int idx) {
			texture_impl->bind(idx);
		}

		int width() const { return width_; }
		int height() const { return height_; }

		driver::texture_interface* get() const { return texture_impl; }

	protected:
		driver::texture_interface* texture_impl = nullptr;
		int width_;
		int height_;
	};
}
