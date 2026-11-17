#pragma once
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>
#include <snk/common/snk_api.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>

namespace rnd
{
	// Defines a pass adds to a material's shader variant. They live in the variant key, so a draw never
	// mutates its shader config to pick a pass variant.
	using pass_define_mask = std::uint32_t;
	inline constexpr pass_define_mask pass_define_none = 0;
	inline constexpr pass_define_mask pass_define_opaque = 1u << 0;

	struct variant_id
	{
		static constexpr std::uint32_t invalid_value = ~0u;

		std::uint32_t value = invalid_value;

		bool is_valid() const noexcept { return value != invalid_value; }
		auto operator<=>(const variant_id&) const noexcept = default;
	};

	// Interns the compile-time part of a shader config (program tags, defines, constants) and hands out a
	// small stable id per distinct variant. The id is valid for the lifetime of the cache.
	class SNK_API shader_config_cache
	{
	public:
		variant_id intern(const shader_config::constant_data& cdata, pass_define_mask passes = pass_define_none);

		// The variant of `source` with another program (z prepass reuses the material's defines).
		variant_id with_program(variant_id source, const shader_config::shader_program_data& program);

		// The config as it is compiled: pass defines are already appended.
		const shader_config::constant_data& resolve(variant_id id) const;

		std::uint64_t key(variant_id id) const;
		std::size_t size() const noexcept { return m_entries.size(); }

	private:
		struct entry
		{
			std::uint64_t key = 0;
			pass_define_mask passes = pass_define_none;
			shader_config::constant_data source;
			shader_config::constant_data resolved;
		};

		const entry& get(variant_id id) const;

		std::vector<entry> m_entries;
		std::unordered_multimap<std::uint64_t, std::uint32_t> m_by_key;
		std::unordered_map<std::uint64_t, variant_id> m_program_memo;
	};

	// A name for a compiled material that is never given to another one, whatever happens to its address.
	SNK_API std::uint64_t next_material_serial() noexcept;

	// A material compiled once and reused across frames: the shader variants it draws with, and the samplers
	// and MaterialParams bytes that do not change per draw. Immutable after it is built; a draw call refers to
	// it by pointer for the frame it was extracted in.
	// The bind group of set 2 is cached by `serial`: a material rebuilt for any reason (its description was
	// marked dirty, a texture was reloaded) is a new compiled_material with a new serial and gets a new group.
	struct compiled_material
	{
		variant_id base;
		variant_id opaque;
		shader_config::runtime_data runtime;
		std::uint64_t serial = next_material_serial();

		std::shared_ptr<const void> source;
		bool skinned = false;
		bool complete = true;
		std::uint64_t texture_generation = 0;

		bool is_current(const void* desc, bool is_skinned, std::uint64_t generation) const noexcept
		{
			return complete && source.get() == desc && skinned == is_skinned && texture_generation == generation;
		}
	};
}
