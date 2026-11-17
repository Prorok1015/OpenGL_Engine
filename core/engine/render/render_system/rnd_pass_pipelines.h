#pragma once
#include <snk/common/mem_allocator.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/render/rnd_render_context.h>

#include <memory_resource>
#include <vector>

namespace rnd
{
	// The polygon mode the scene passes draw a camera's geometry with.
	inline driver::polygon_mode scene_polygon(const camera_render_data_t& camera)
	{
		return camera.wireframe ? driver::polygon_mode::LINE : driver::polygon_mode::FILL;
	}

	// The pipelines one render pass draws with. The pass states what is fixed for all its draws (fixed-function
	// state and the formats of its targets); each material variant adds its program. The driver's pipeline cache
	// makes a pipeline once per distinct description, this only spares it a lookup for every draw.
	// Frame temporary: it keeps a frame-allocator vector, do not store it across frames.
	class pass_pipelines
	{
	public:
		pass_pipelines(driver::driver_interface& drv, render_context& context, driver::pipeline_desc base)
			: m_drv(drv)
			, m_context(context)
			, m_base(std::move(base))
			, m_by_variant(ds::frame_allocator())
		{
			m_by_variant.reserve(m_context.variants().size() + 8);
		}

		// Binds the pipeline of a variant, unless it is the one bound already. False when the variant has no
		// program: there is nothing to draw it with.
		bool bind(variant_id variant)
		{
			return bind(get(variant));
		}

		// The same for a program that does not come from a material variant.
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
				slot = build(m_context.program(variant));
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
		render_context& m_context;
		driver::pipeline_desc m_base;
		std::pmr::vector<driver::pipeline_handle> m_by_variant;
		driver::pipeline_handle m_bound;
	};
}
