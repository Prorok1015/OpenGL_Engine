#pragma once
#include <snk/common/snk_api.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/shader/rnd_shader_config_cache.h>
#include <snk/render/rnd_material_desc.h>
#include <snk/render/rnd_shader_problem.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace res { class resource_system; }

namespace rnd
{
	class texture_manager;
	struct material_compiler_interface;
	struct material_shader_state;

	// The material instances of the renderer: a material_desc compiled for drawing (shader variants, resolved
	// maps, MaterialParams bytes; its set 2 group is made from these by material_groups when a draw binds it).
	// A draw names its instance by a material_instance_id, an index into this cache, so the packet stays plain
	// data and extraction does not compile anything or ask for a texture.
	//
	// An instance is made the first time a (description, skinned) pair is asked for and is compiled again, in
	// the same slot (the id stays valid), only when
	//   - the caller says the description changed (scn::material_desc_component::dirty),
	//   - a texture was reloaded or a render target resized (texture_manager::generation()), or
	//   - a map it asked for was still loading when it was compiled, or
	//   - its shader desc changed on disk (#1272): the old instance draws while the new shader loads, and keeps drawing
	//     if it fails to load.
	// An instance keeps the shader handle it compiled with, so a later rebuild does not bring back a replaced shader.
	// A shader that failed is logged and listed in problems() once while the material stays in the cache: one unused for
	// more than grace_frames loses its problem and is reported again when it comes back.
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

		// `resources`: where the shaders of the instances are watched for changes; nullptr watches nothing.
		material_instance_cache(shader_config_cache& variants, texture_manager& textures, res::resource_system* resources = nullptr);
		~material_instance_cache();
		material_instance_cache(const material_instance_cache&) = delete;
		material_instance_cache& operator=(const material_instance_cache&) = delete;

		// Call once per frame before extraction: sets the compiler the frame compiles with and drops the instances
		// that were not asked for during grace_frames frames. Ids of a finished frame are not valid after this.
		void begin_frame(material_compiler_interface* compiler);

		// The instance of `desc` for a skinned or static mesh. `desc_changed` says the description was edited in
		// place since the last call; a description replaced by another object needs no flag, it is another key.
		material_instance_id acquire(const std::shared_ptr<material_desc>& desc, bool skinned, bool desc_changed);

		// The instance an id from acquire() names; valid until the next begin_frame.
		const compiled_material& get(material_instance_id id) const;
		bool contains(material_instance_id id) const noexcept;

		// Instances alive now.
		std::size_t size() const noexcept;
		// Compilations done so far; grows only when an instance is made or rebuilt.
		std::uint64_t compilations() const noexcept;

		// The problems of materials, for the Problems panel: a shader that failed to load ("shader/load") and a transparent or
		// mix material whose shader has no transparent path (k_no_transparent_path_code), each once per (shader, material).
		// The generation grows whenever they change.
		std::vector<shader_problem> problems() const;
		std::uint64_t problems_generation() const noexcept;

	private:
		struct key
		{
			const void* desc = nullptr;
			bool skinned = false;
			bool operator==(const key& other) const;
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
			// What the instance compiles with, not desc->shader: after a reload the desc still holds the old block.
			res::res_handle<shader_desc> shader;
			// A fresh request of `shader` after its file changed; adopted once it has loaded.
			res::res_handle<shader_desc> pending;
			bool shader_changed = false;
			// The last reload failed: the instance keeps the old shader, and its problem stays until a reload succeeds.
			bool reload_failed = false;
			// The desc's tag at the last compile: what its problem is filed under, also once the instance is dropped.
			std::string material_tag;
		};

		void compile(slot& s, const std::shared_ptr<material_desc>& desc, bool skinned);
		// True when the changed shader has loaded and the slot must be compiled with it.
		bool reload_shader(slot& s);
		void set_shader(slot& s, res::res_handle<shader_desc> shader);
		void on_shader_changed(const res::tag& shader);
		void report_problem(const res::tag& shader, std::string_view material, const std::string& error, bool reload);
		// Every problem of the pair, or the one of `code` only.
		void clear_problem(const res::tag& shader, std::string_view material, std::string_view code = {});
		// A material of the transparent or mix queue whose shader has no transparent path: drawn as before, reported (#1440).
		void check_transparent_path(const slot& s, const material_desc& desc, const material_shader_state& shader);
		// Whether a live slot compiles `material` with `shader` (and, with reload_failed_only, its last reload failed).
		bool any_slot(const res::tag& shader, std::string_view material, bool reload_failed_only) const;

		shader_config_cache& m_variants;
		texture_manager& m_textures;
		res::resource_system* m_resources = nullptr;
		material_compiler_interface* m_compiler = nullptr;
		std::vector<slot> m_slots;
		std::vector<std::uint32_t> m_free;
		std::unordered_map<key, std::uint32_t, key_hash> m_by_key;
		std::uint64_t m_frame = 0;
		std::uint64_t m_compilations = 0;
		// Slots per watched shader tag.
		std::unordered_map<res::tag, std::size_t> m_shader_users;
		std::vector<shader_problem> m_problems;
		std::uint64_t m_problems_generation = 0;
	};
}
