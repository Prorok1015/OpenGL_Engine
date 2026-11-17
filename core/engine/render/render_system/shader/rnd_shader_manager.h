#pragma once
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <snk/resource/res_tag.h>
#include <snk/render/shader/rnd_scene_shader_desc.h>
#include <snk/render/shader/rnd_shader_config_cache.h>

#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_gpu_blocks.h>

namespace rnd
{
	// The programs of the shader variants, and set 0 (the camera and the lights). Internal to the renderer: the
	// passes get programs and set 0 through render_context.
	class SNK_API ShaderManager
	{
	public:
		ShaderManager(driver::driver_interface* ptr, res::resource_system& res_sys);
		~ShaderManager();

		ShaderManager(const ShaderManager&) = delete;
		ShaderManager(ShaderManager&&) = delete;
		ShaderManager& operator= (const ShaderManager&) = delete;
		ShaderManager& operator= (ShaderManager&&) = delete;

		void clear_cache() const {
			m_programs.clear();
		}

		shader_config_cache& variants() const { return m_variants; }

		// The program of a variant, compiled on its first use; null when it does not compile. A variant whose
		// source cannot be prepared or compiled is tried once, not again at every draw, until a file its source
		// is made of changes (a stage or an included file): the next call compiles it again. A program that does
		// not compile after such a change is not put in place of the one that did.
		driver::shader_interface* program(variant_id variant) const { return require_program(variant); }

		// Frees the programs that were replaced since the last call, with the pipelines made for them. At the start
		// of a frame, when no pass_pipelines is alive to hold one of those pipelines.
		void begin_frame() const;

		const set_layouts& layouts() const { return m_layouts; }

		void update_global_uniform(const global_params& val) const;
		void update_global_sun(const lights_params& val) const;
		// Binds set 0 (Matrices, GlobalLights); inside a render pass, before its draws.
		void bind_frame() const;
	private:
		struct program_slot
		{
			std::unique_ptr<rnd::driver::shader_interface> program;
			bool failed = false;
			bool stale = false;	// a file of its source changed since it was compiled
		};

		driver::driver_interface* drv = nullptr;
		res::resource_system& m_res_sys;
		set_layouts m_layouts;
		driver::buffer_handle m_matrices;
		driver::buffer_handle m_lights;
		driver::bind_group_handle m_frame_group;
		rnd::driver::shader_interface* require_program(variant_id variant) const;
		// Remembers that `variant` is made of `files` and starts watching the ones not watched yet.
		void track_files(variant_id variant, const std::vector<res::tag>& files) const;
		void on_file_changed(const res::tag& file) const;

		mutable shader_config_cache m_variants;
		mutable std::vector<program_slot> m_programs;
		mutable std::unordered_map<res::tag, std::vector<std::uint32_t>> m_users;	// file -> the variants made of it
		// A replaced program waits here for begin_frame: a pass may still draw with it in the frame it was replaced in.
		mutable std::vector<std::unique_ptr<rnd::driver::shader_interface>> m_retired;
	};
}
