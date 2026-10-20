#pragma once
#include <snk/common/snk_api.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_texture_interface.h>
#include <snk/driver/rnd_texture_owner.h>
#include <snk/render/rnd_bindings.h>
#include <snk/render/shader/rnd_material_params.h>
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
	// A group is built the first time a material with its contents is drawn: the params bytes go into a buffer
	// once, the textures the material resolved (a placeholder for a map it does not have, or one that is still
	// loading) into the group. Nothing is written after that. The cache key is the contents (the params bytes
	// and the three textures, placeholders included), compared in full: materials that are equal share one
	// buffer and group however many entities have them, and a compiled_material rebuilt with the same contents
	// (a texture still loading, a render target resized) allocates nothing. A change of params or of a texture
	// is a different key and so a new group. Groups not used for `grace_frames` frames are destroyed.
	//
	// This is the part of the material instance (#125) that lives on the GPU.
	class SNK_API material_groups
	{
	public:
		// How long an unused group is kept: a viewport that renders every few frames, or a mesh at the edge of
		// the frustum, does not make the groups again for every draw.
		static constexpr std::uint64_t grace_frames = 8;

		explicit material_groups(driver::driver_interface& drv);
		~material_groups();
		material_groups(const material_groups&) = delete;
		material_groups& operator=(const material_groups&) = delete;

		// Call once per frame, before the passes: drops the groups that were not used for grace_frames frames.
		void begin_frame();

		// Binds set 2 for `material`, inside a render pass.
		void bind(const compiled_material& material);

		// Persistent groups made so far; grows only with contents that were not cached.
		std::size_t groups_created() const { return m_groups_created; }
		// Distinct contents that have a group now.
		std::size_t cached() const { return m_entries.size(); }
		const driver::bind_group_layout_handle& layout() const { return m_layout; }

	private:
		struct content_key
		{
			std::array<std::byte, sizeof(material_params)> block{};
			// Serials, not addresses: a texture destroyed and replaced at the same address is another texture.
			std::array<std::uint64_t, bindings::material_texture_count> textures{};

			bool operator==(const content_key&) const = default;
		};

		struct content_hash
		{
			std::size_t operator()(const content_key& key) const noexcept;
		};

		struct entry
		{
			driver::buffer_handle params;
			driver::bind_group_handle group;
			std::uint64_t last_used = 0;
		};

		entry& acquire(const compiled_material& material);
		using texture_set = std::array<driver::texture_interface*, bindings::material_texture_count>;
		// The textures the group binds (placeholders for the missing ones) and the key they and the params make.
		content_key key_of(const compiled_material& material, texture_set& textures) const;
		void destroy(entry& e);

		driver::driver_interface& m_drv;
		driver::bind_group_layout_handle m_layout;
		std::array<driver::texture_owner, bindings::material_texture_count> m_placeholders;
		std::unordered_map<content_key, entry, content_hash> m_entries;
		std::uint64_t m_frame = 0;
		std::size_t m_groups_created = 0;
	};
}
