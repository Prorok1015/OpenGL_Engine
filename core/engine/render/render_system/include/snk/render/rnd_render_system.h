#pragma once
#include <snk/common/common.h>
#include <snk/driver/rnd_driver_interface.h>

#include <snk/render/rnd_render_pass_interface.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace desc { class desc_system; }
namespace res { class resource_system; }

// The managers are the renderer's own: the header only names them. A caller that needs the texture manager
// includes <snk/render/texture/rnd_texture_manager.h>; the shader and geometry managers are not public.
namespace rnd
{
	class texture_manager;
	class ShaderManager;
	class geom_manager;
	class per_draw_data;
	class material_groups;
	class material_instance_cache;
	class skinning_manager;
	struct material_compiler_interface;
}

namespace rnd
{
	using RENDER_MODE = driver::RENDER_MODE;

	class render_system
	{
	public:
		render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d, res::resource_system& res);
		~render_system();
		render_system(const render_system&) = delete;
		render_system(render_system&&) = delete;
		render_system& operator= (const render_system&) = delete;
		render_system& operator= (render_system&&) = delete;

		const rnd::texture_manager& get_texture_manager() const { return *m_textures; }
		rnd::texture_manager& get_texture_manager() { return *m_textures; }

		// The slots of the render targets the passes make (stable per tag, the generation grows on re-creation).
		const rnd::render_targets::registry& get_render_targets() const { return *m_targets; }
		rnd::render_targets::registry& get_render_targets() { return *m_targets; }

		// Runs the passes over the packets of `context`. Each pass gets a render_context for the frame, which
		// hides the managers behind it.
		void render_frame(frame_context& context) const;

		// Gives the extract stage the services it needs (frame_context). Call once per frame before
		// build_frame / render_frame.
		void begin_frame(frame_context& context);

		void add_render_pass(std::shared_ptr<render_pass_interface> pass) {
			render_passes.push_back(pass);
		}

		void remove_render_pass(std::shared_ptr<render_pass_interface> pass) {
			render_passes.erase(std::remove(render_passes.begin(), render_passes.end(), pass));
		}

		rnd::driver::driver_interface* get_driver() const { return drv.get(); }

		void set_skinning_manager(rnd::skinning_manager* sm) { m_skinning_manager = sm; }
		rnd::skinning_manager* get_skinning_manager() const { return m_skinning_manager; }

		void set_material_compiler(std::shared_ptr<material_compiler_interface> mc) { m_material_compiler = std::move(mc); }
		material_compiler_interface* get_material_compiler() const { return m_material_compiler.get(); }

		// The cache the draws' material ids refer to (valid between begin_frame and the next one).
		material_instance_cache& material_instances() { return *m_instances; }

	private:
		rnd::skinning_manager* m_skinning_manager = nullptr;
		std::shared_ptr<material_compiler_interface> m_material_compiler;
		std::unique_ptr<rnd::driver::driver_interface> drv = nullptr;
		std::unique_ptr<rnd::texture_manager> m_textures;
		std::unique_ptr<rnd::render_targets::registry> m_targets;
		std::unique_ptr<rnd::ShaderManager> m_shaders;
		std::unique_ptr<rnd::geom_manager> m_geometry;
		std::unique_ptr<rnd::per_draw_data> m_draw_data;
		std::unique_ptr<rnd::material_groups> m_materials;
		std::unique_ptr<rnd::material_instance_cache> m_instances;

		std::vector<std::shared_ptr<render_pass_interface>> render_passes;
	};
}
