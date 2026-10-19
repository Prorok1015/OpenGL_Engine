#pragma once
#include <snk/driver/rnd_handle.h>
#include <snk/driver/rnd_render_pass.h>
#include <snk/common/ds/ds_fixed_vector.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace rnd::driver
{
	struct buffer_tag;
	struct bind_group_layout_tag;
	struct bind_group_tag;
	using buffer_handle = handle<buffer_tag>;
	using bind_group_layout_handle = handle<bind_group_layout_tag>;
	using bind_group_handle = handle<bind_group_tag>;

	// How many frames a driver keeps apart, so the CPU can write one while the GPU still reads another.
	inline constexpr std::uint32_t frames_in_flight = 3;

	enum class buffer_usage : std::uint8_t { vertex, index, uniform, storage };

	// How often the contents change. `per_frame` buffers are written once per frame and the driver keeps
	// `frames_in_flight` copies, so one handle stays valid while the copy it reads rotates at begin_frame.
	enum class buffer_update : std::uint8_t { static_data, dynamic, per_frame };

	struct buffer_desc
	{
		std::size_t size = 0;
		buffer_usage usage = buffer_usage::uniform;
		buffer_update update = buffer_update::dynamic;
		const char* debug_name = nullptr;
	};

	enum class binding_type : std::uint8_t { ubo, ssbo, texture };
	inline constexpr std::size_t binding_type_count = 3;

	enum class shader_stage : std::uint8_t
	{
		vertex = 1,
		fragment = 2,
		geometry = 4,
		all = 7,
	};

	constexpr shader_stage operator|(shader_stage a, shader_stage b) noexcept
	{
		return static_cast<shader_stage>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
	}

	// The sets of the engine's shaders (ARCHITECTURE_REVIEW 9.4).
	inline constexpr std::uint32_t max_bind_sets = 4;
	inline constexpr std::uint32_t frame_set = 0;	// frame and view: Matrices, GlobalLights
	inline constexpr std::uint32_t pass_set = 1;	// inputs of a pass
	inline constexpr std::uint32_t material_set = 2;
	inline constexpr std::uint32_t object_set = 3;	// per draw: ObjectData, bones

	// (set, slot, type) -> the binding number the backend's API uses. Slots are laid out per resource type, one
	// block of bindings per set, so a type's bindings are as many as the table says and never `set * 16 + slot`:
	// GL guarantees only 8 storage buffer bindings. The shader preprocessor expands SNK_UBO/SNK_SSBO/SNK_TEX
	// with it, the validation checks bind groups against it.
	class binding_table
	{
	public:
		// Slots a set has for each binding_type, in the order of the enum.
		using set_slots = std::array<std::uint8_t, binding_type_count>;

		constexpr explicit binding_table(const std::array<set_slots, max_bind_sets>& slots) noexcept : m_slots(slots) {}

		constexpr std::uint32_t slots(std::uint32_t set, binding_type type) const noexcept
		{
			return set < max_bind_sets ? m_slots[set][static_cast<std::size_t>(type)] : 0;
		}

		constexpr bool contains(std::uint32_t set, std::uint32_t slot, binding_type type) const noexcept
		{
			return slot < slots(set, type);
		}

		// Empty for a set or slot outside the table.
		constexpr std::optional<std::uint32_t> resolve(std::uint32_t set, std::uint32_t slot, binding_type type) const noexcept
		{
			if (!contains(set, slot, type)) {
				return std::nullopt;
			}
			std::uint32_t first = 0;
			for (std::uint32_t s = 0; s < set; ++s) {
				first += slots(s, type);
			}
			return first + slot;
		}

		// Bindings of one type over all sets.
		constexpr std::uint32_t total(binding_type type) const noexcept
		{
			std::uint32_t sum = 0;
			for (std::uint32_t s = 0; s < max_bind_sets; ++s) {
				sum += slots(s, type);
			}
			return sum;
		}

	private:
		std::array<set_slots, max_bind_sets> m_slots;
	};

	// The table of backends that number each resource type on its own (GL, and the null backend that models it):
	// 8 uniform blocks (2 per set), 2 storage buffers (object set) and 12 texture units (pass set 4, material set 8).
	inline constexpr binding_table per_type_binding_table{ { {
		binding_table::set_slots{ 2, 0, 0 },
		binding_table::set_slots{ 2, 0, 4 },
		binding_table::set_slots{ 2, 0, 8 },
		binding_table::set_slots{ 2, 2, 0 },
	} } };

	struct binding_decl
	{
		binding_type type = binding_type::ubo;
		std::uint32_t slot = 0;
		shader_stage stages = shader_stage::all;
		// The buffer range is moved by an offset given at set_bind_group (uniform buffers only).
		bool dynamic = false;

		friend bool operator==(const binding_decl&, const binding_decl&) = default;
	};

	inline constexpr std::size_t max_bindings_per_group = 16;

	struct bind_group_layout_desc
	{
		ds::fixed_vector<binding_decl, max_bindings_per_group> entries;

		friend bool operator==(const bind_group_layout_desc&, const bind_group_layout_desc&) = default;
	};

	// What a bind group puts at one binding of its layout: the (type, slot) pair names the binding.
	struct binding_res
	{
		binding_type type = binding_type::ubo;
		std::uint32_t slot = 0;
		// ubo and ssbo: a range of `buffer`. A dynamic entry adds the offset given at set_bind_group to `offset`.
		buffer_handle buffer;
		std::size_t offset = 0;
		std::size_t size = 0;
		// texture: the texture with the sampling state it carries.
		texture_handle texture = nullptr;

		friend bool operator==(const binding_res&, const binding_res&) = default;
	};

	struct bind_group_desc
	{
		bind_group_layout_handle layout;
		ds::fixed_vector<binding_res, max_bindings_per_group> entries;
	};

	// A transient group lives until the end of the frame it was made in (the driver's next begin_frame).
	enum class lifetime : std::uint8_t { persistent, transient };

	// set_bind_group takes one offset per dynamic entry of the group's layout, in the order of the layout's entries.
	inline std::size_t dynamic_entry_count(const bind_group_layout_desc& layout)
	{
		std::size_t count = 0;
		for (const binding_decl& decl : layout.entries) {
			count += decl.dynamic ? 1 : 0;
		}
		return count;
	}

	struct bind_group_layout_desc_hash
	{
		std::size_t operator()(const bind_group_layout_desc& desc) const noexcept
		{
			std::uint64_t h = 0xcbf29ce484222325ull;
			const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; h ^= h >> 29; };
			mix(desc.entries.size());
			for (const binding_decl& decl : desc.entries) {
				mix(static_cast<std::uint64_t>(decl.slot) | static_cast<std::uint64_t>(decl.type) << 32
					| static_cast<std::uint64_t>(decl.stages) << 40 | static_cast<std::uint64_t>(decl.dynamic) << 48);
			}
			return static_cast<std::size_t>(h);
		}
	};
}
