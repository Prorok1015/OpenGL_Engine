#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/driver/rnd_texture_owner.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <unordered_map>
#include <functional>
#include <snk/common/snk_api.h>

namespace res { class resource_system; }

namespace rnd
{
	class SNK_API texture_manager
	{
	public:
		// Returns an empty owner while the texture cannot be made yet (the manager asks again later).
		using loader_fn = std::function<driver::texture_owner(driver::driver_interface* drv, const rnd::texture_desc& txm)>;
		texture_manager(driver::driver_interface* driver, desc::desc_system& d, res::resource_system& res_sys)
			: desc_system(d)
			, m_res_sys(res_sys)
			, drv(driver)
		{}

		// dllexport instantiates the implicit copy members, which cannot compile with move-only values
		texture_manager(const texture_manager&) = delete;
		texture_manager& operator=(const texture_manager&) = delete;

		driver::texture_interface* require_texture(const res::tag& tag);
		driver::texture_interface* generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data);
		driver::texture_interface* generate_texture(const res::tag& tag, rnd::driver::texture_header header);

		driver::texture_interface* find(const res::tag& tag) const;
		void remove(const res::tag& tag);

		const std::unordered_map<res::tag, driver::texture_owner>& get_cache() const 
		{
			return cache;
		}

		void clear_cache();

		// Bumped whenever a texture may have been destroyed, so anything that kept texture pointers
		// across frames knows to re-resolve them.
		std::uint64_t generation() const noexcept { return m_generation; }

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
		mutable std::unordered_map<res::tag, driver::texture_owner> cache;
		std::unordered_map<res::tag, res::res_handle<rnd::texture_desc>> loading;
		desc::desc_system& desc_system;
		res::resource_system& m_res_sys;
		driver::driver_interface* drv = nullptr;
		std::uint64_t m_generation = 0;
	};

}