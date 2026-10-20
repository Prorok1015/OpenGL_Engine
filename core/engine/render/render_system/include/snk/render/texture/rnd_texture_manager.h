#pragma once
#include <snk/common/common.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/driver/rnd_texture_owner.h>
#include <snk/render/texture/rnd_texture_desc.h>
#include <snk/render/texture/rnd_texture_id.h>
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>
#include <snk/common/snk_api.h>

namespace res { class resource_system; }

namespace rnd
{
	// The textures of the renderer, by tag. A tag has a slot (a texture_id) from the first time it is asked for:
	// the slot keeps its id while the texture behind it is loaded, replaced (reload, render-target resize) or
	// removed, and its generation grows with each of those. Everything that holds a texture across frames holds
	// the id and asks binding() for the handle when it draws.
	class SNK_API texture_manager final : public texture_resolver
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

		// The slot of `tag` (made on the first ask). Starts the load when the texture is not there yet; the slot
		// has no handle until it is loaded (is_ready), and its generation grows when it arrives.
		texture_id require(const res::tag& tag);
		// Makes the texture of `tag` from `header` if the slot has none; the slot keeps the texture it has otherwise.
		texture_id generate(const res::tag& tag, rnd::driver::texture_header header);
		// The slot of `tag` if it was asked for, otherwise no id. The slot may be empty.
		texture_id id_of(const res::tag& tag) const;
		// Whether the slot has a texture now.
		bool is_ready(texture_id id) const;

		texture_binding binding(texture_id id) const override;

		// Bridge for the render targets, the capture service and the editor until they hold ids (R7b3, R7c): the
		// backend object behind the tag, null while the slot has none.
		driver::texture_interface* generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data);
		driver::texture_interface* generate_texture(const res::tag& tag, rnd::driver::texture_header header);
		driver::texture_interface* find(const res::tag& tag) const;

		// Destroys the texture of `tag`; the slot and its id stay (its generation grows).
		void remove(const res::tag& tag);
		// Destroys every texture that is not a memory:// one; the slots stay.
		void clear_cache();

		// Bumped whenever a texture may have been destroyed, so anything that kept the result of a compilation
		// across frames knows to look again.
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
		struct slot
		{
			res::tag tag;
			driver::texture_owner owner;
			std::uint32_t generation = 1;
		};

		texture_id slot_of(const res::tag& tag);
		const slot* find_slot(texture_id id) const noexcept;
		// Puts a made texture into its slot: the generation grows.
		void fill(slot& s, driver::texture_owner texture);
		// Destroys the texture of a slot and keeps the slot.
		void empty(slot& s);

		mutable std::unordered_map<std::string, loader_fn> loaders;
		std::unordered_map<res::tag, texture_id> m_ids;
		std::vector<slot> m_slots; // m_slots[id.index - 1]
		std::unordered_map<res::tag, res::res_handle<rnd::texture_desc>> loading;
		desc::desc_system& desc_system;
		res::resource_system& m_res_sys;
		driver::driver_interface* drv = nullptr;
		std::uint64_t m_generation = 0;
	};

}
