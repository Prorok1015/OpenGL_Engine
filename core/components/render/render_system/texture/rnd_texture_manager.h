#pragma once
#include <common.h>
#include "res_tag.h"
#include "rnd_texture.h"

#include <rnd_driver_interface.h>
#include <rnd_texture_interface.h>

namespace rnd
{
	class TextureManager
	{
	public:
		TextureManager(driver::driver_interface* driver)
			: drv(driver) {}

		driver::texture_interface* require_texture(const res::tag& tag);
		driver::texture_interface* require_cubemap_texture(const std::vector<res::tag>& tags);
		driver::texture_interface* generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data);
		driver::texture_interface* generate_texture(const res::tag& tag, rnd::driver::texture_header header);

		driver::texture_interface* find(const res::tag& tag) const;
		void remove(const res::tag& tag);

		const std::unordered_map<res::tag, std::unique_ptr<driver::texture_interface>, res::tag::hash>& get_cache() const 
		{
			return cache;
		}

		void clear_cache();
	protected:
		mutable std::unordered_map<res::tag, std::unique_ptr<driver::texture_interface>, res::tag::hash> cache;
		driver::driver_interface* drv = nullptr;
	};

}