#pragma once
#include "common.h"
#include "res_tag.h"
#include "desc_system.h"
#include "rnd_driver_interface.h"
#include "rnd_texture_interface.h"
#include "texture/rnd_texture_desc.h"
#include <unordered_map>
#include <functional>

namespace rnd
{
	class texture_manager
	{
	public:
		using loader_fn = std::function<std::unique_ptr<driver::texture_interface>(driver::driver_interface* drv, const rnd::texture_desc& txm)>;
		texture_manager(driver::driver_interface* driver, desc::desc_system& d)
			: drv(driver)
			, desc_system(d)
		{}

		driver::texture_interface* require_texture(const res::tag& tag);
		driver::texture_interface* require_cubemap_texture(const std::vector<res::tag>& tags);
		driver::texture_interface* generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data);
		driver::texture_interface* generate_texture(const res::tag& tag, rnd::driver::texture_header header);

		driver::texture_interface* find(const res::tag& tag) const;
		void remove(const res::tag& tag);

		const std::unordered_map<res::tag, std::unique_ptr<driver::texture_interface>>& get_cache() const 
		{
			return cache;
		}

		void clear_cache();

		void register_loader(const std::string& txm_type, loader_fn fn)
		{
			loaders[txm_type] = std::move(fn);
		}

		void unregister_loader(const std::string& txm_type)
		{
			loaders.erase(txm_type);
		}
	protected:
		mutable std::unordered_map<std::string, loader_fn> loaders;
		mutable std::unordered_map<res::tag, std::unique_ptr<driver::texture_interface>> cache;
		std::unordered_map<res::tag, res::res_handle<rnd::texture_desc>> loading;
		desc::desc_system& desc_system;
		driver::driver_interface* drv = nullptr;
	};

}