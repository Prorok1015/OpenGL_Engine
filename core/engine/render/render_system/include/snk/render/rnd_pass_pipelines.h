#pragma once
#include <snk/common/mem_allocator.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/shader/rnd_shader_manager.h>

#include <memory_resource>
#include <vector>

namespace rnd
{
	// The pipelines one render pass draws with. The pass states what is fixed for all its draws (fixed-function
	// state and the formats of its targets); each material variant adds its program. The driver's pipeline cache
	// makes a pipeline once per distinct description, this only spares it a lookup for every draw.
	// Frame temporary: it keeps a frame-allocator vector, do not store it across frames.
	class pass_pipelines
	{
	public:
		pass_pipelines(driver::driver_interface& drv, const ShaderManager& shaders, driver::pipeline_desc base)
			: m_drv(drv)
			, m_shaders(shaders)
			, m_base(std::move(base))
			, m_by_variant(ds::frame_allocator())
		{
			m_by_variant.reserve(m_shaders.variants().size() + 8);
		}

		// Binds the pipeline of a variant, unless it is the one bound already. False when the variant has no
		// program: there is nothing to draw it with.
		bool bind(variant_id variant)
		{
			return bind(get(variant));
		}

		// The same for a program that does not come from the shader manager.
		bool bind(const driver::shader_interface* program)
		{
			return bind(build(program));
		}

	private:
		bool bind(driver::pipeline_handle pipeline)
		{
			if (!pipeline) {
				return false;
			}
			if (pipeline != m_bound) {
				m_drv.bind_pipeline(pipeline);
				m_bound = pipeline;
			}
			return true;
		}

		// Null when the variant has no program.
		driver::pipeline_handle get(variant_id variant)
		{
			if (!variant.is_valid()) {
				return {};
			}
			if (variant.value >= m_by_variant.size()) {
				m_by_variant.resize(variant.value + 1);
			}
			driver::pipeline_handle& slot = m_by_variant[variant.value];
			if (!slot) {
				slot = build(m_shaders.program(variant));
			}
			return slot;
		}

		driver::pipeline_handle build(const driver::shader_interface* program)
		{
			if (!program) {
				return {};
			}
			driver::pipeline_desc desc = m_base;
			desc.shader = program;
			return m_drv.create_pipeline(desc);
		}

		driver::driver_interface& m_drv;
		const ShaderManager& m_shaders;
		driver::pipeline_desc m_base;
		std::pmr::vector<driver::pipeline_handle> m_by_variant;
		driver::pipeline_handle m_bound;
	};
}
