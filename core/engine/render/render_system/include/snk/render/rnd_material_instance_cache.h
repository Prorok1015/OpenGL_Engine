#pragma once
#include <snk/common/snk_api.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/scene/scn_material_desc.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rnd
{
	class texture_manager;
	struct material_compiler_interface;

	// The material instances of the renderer: a material_desc compiled for drawing (shader variants, resolved
	// maps, MaterialParams bytes; its set 2 group is made from these by material_groups when a draw binds it).
	// A draw names its instance by a material_instance_id, an index into this cache, so the packet stays plain
	// data and extraction does not compile anything or ask for a texture.
	//
	// An instance is made the first time a (description, skinned) pair is asked for and is compiled again, in
	// the same slot (the id stays valid), only when
	//   - the caller says the description changed (scn::material_desc_component::dirty),
	//   - a texture was reloaded or a render target resized (texture_manager::generation()), or
	//   - a map it asked for was still loading when it was compiled.
	// An instance is compiled at most once per frame, however many draws and cameras ask for it. One that has not
	// been asked for during `grace_frames` frames is dropped (its slot is reused).
	//
	// Not thread safe: the extract stage and begin_frame run on the frame thread.
	class SNK_API material_instance_cache
	{
	public:
		// How long an unused instance is kept: a viewport that renders every few frames does not recompile its
		// materials for every frame it draws.
		static constexpr std::uint64_t grace_frames = 8;

		material_instance_cache(shader_config_cache& variants, texture_manager& textures);
		material_instance_cache(const material_instance_cache&) = delete;
		material_instance_cache& operator=(const material_instance_cache&) = delete;

		// Call once per frame before extraction: sets the compiler the frame compiles with and drops the instances
		// that were not asked for during grace_frames frames. Ids of a finished frame are not valid after this.
		void begin_frame(material_compiler_interface* compiler);

		// The instance of `desc` for a skinned or static mesh. `desc_changed` says the description was edited in
		// place since the last call; a description replaced by another object needs no flag, it is another key.
		material_instance_id acquire(const std::shared_ptr<scn::material_desc>& desc, bool skinned, bool desc_changed);

		// The instance an id from acquire() names; valid until the next begin_frame.
		const compiled_material& get(material_instance_id id) const;
		bool contains(material_instance_id id) const noexcept;

		// Instances alive now.
		std::size_t size() const noexcept { return m_by_key.size(); }
		// Compilations done so far; grows only when an instance is made or rebuilt.
		std::uint64_t compilations() const noexcept { return m_compilations; }

	private:
		struct key
		{
			const void* desc = nullptr;
			bool skinned = false;
			bool operator==(const key&) const = default;
		};

		struct key_hash
		{
			std::size_t operator()(const key& k) const noexcept
			{
				return std::hash<const void*>{}(k.desc) * 2u + (k.skinned ? 1u : 0u);
			}
		};

		struct slot
		{
			// Holds the description alive too (compiled_material::source), so its address cannot be reused while
			// the instance is cached.
			std::shared_ptr<compiled_material> material;
			std::uint64_t last_used = 0;
			std::uint64_t built_frame = 0;
		};

		void compile(slot& s, const std::shared_ptr<scn::material_desc>& desc, bool skinned);

		shader_config_cache& m_variants;
		texture_manager& m_textures;
		material_compiler_interface* m_compiler = nullptr;
		std::vector<slot> m_slots;
		std::vector<std::uint32_t> m_free;
		std::unordered_map<key, std::uint32_t, key_hash> m_by_key;
		std::uint64_t m_frame = 0;
		std::uint64_t m_compilations = 0;
	};
}
