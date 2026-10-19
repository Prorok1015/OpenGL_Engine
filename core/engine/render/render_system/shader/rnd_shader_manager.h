#pragma once
#include <snk/common/common.h>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <memory>
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
		// source cannot be prepared or compiled is tried once, not again at every draw.
		driver::shader_interface* program(variant_id variant) const { return require_program(variant); }

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
		};

		driver::driver_interface* drv = nullptr;
		res::resource_system& m_res_sys;
		set_layouts m_layouts;
		driver::buffer_handle m_matrices;
		driver::buffer_handle m_lights;
		driver::bind_group_handle m_frame_group;
		rnd::driver::shader_interface* require_program(variant_id variant) const
		{
			if (variant.value >= m_programs.size()) {
				m_programs.resize(variant.value + 1);
			}

			auto& slot = m_programs[variant.value];
			if (!slot.program && !slot.failed) {
				auto source = shader_config::load(m_variants.resolve(variant), m_res_sys, drv->bindings());
				if (source.ok()) {
					slot.program = drv->create_shader(source.headers);
				}
				slot.failed = !slot.program;
			}
			return slot.program.get();
		}

		mutable shader_config_cache m_variants;
		mutable std::vector<program_slot> m_programs;
	};
}
