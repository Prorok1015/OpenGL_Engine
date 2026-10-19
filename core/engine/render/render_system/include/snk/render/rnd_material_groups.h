#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/render/rnd_bindings.h>
#include <snk/render/shader/rnd_shader_config_cache.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace rnd
{
	// What draws read from set 2 (material): the MaterialParams block and the maps, in one bind group per
	// compiled material.
	//
	// A group is built the first time its material is drawn: the params bytes go into a buffer once, the
	// textures the material resolved (a placeholder for a map it does not have, or one that is still loading)
	// into the group. Nothing is written after that. A compiled_material is immutable and rebuilt whenever its
	// description or a texture changes (including a hot reload), and every build has its own serial, so the
	// cache key cannot name stale contents. Groups of materials that were not drawn for a frame are destroyed.
	//
	// This is the part of the material instance (#125) that lives on the GPU.
	class SNK_API material_groups
	{
	public:
		explicit material_groups(driver::driver_interface& drv);
		~material_groups();
		material_groups(const material_groups&) = delete;
		material_groups& operator=(const material_groups&) = delete;

		// Call once per frame, before the passes: drops the groups of materials the last frame did not draw.
		void begin_frame();

		// Binds set 2 for `material`, inside a render pass.
		void bind(const compiled_material& material);

		// Persistent groups made so far; grows only with materials that were not cached.
		std::size_t groups_created() const { return m_groups_created; }
		// Materials that have a group now.
		std::size_t cached() const { return m_entries.size(); }
		const driver::bind_group_layout_handle& layout() const { return m_layout; }

	private:
		struct entry
		{
			driver::buffer_handle params;
			driver::bind_group_handle group;
			std::uint64_t last_used = 0;
		};

		entry& acquire(const compiled_material& material);
		void destroy(entry& e);

		driver::driver_interface& m_drv;
		driver::bind_group_layout_handle m_layout;
		std::array<std::unique_ptr<driver::texture_interface>, bindings::material_texture_count> m_placeholders;
		std::unordered_map<std::uint64_t, entry> m_entries;
		std::uint64_t m_frame = 0;
		std::size_t m_groups_created = 0;
	};
}
