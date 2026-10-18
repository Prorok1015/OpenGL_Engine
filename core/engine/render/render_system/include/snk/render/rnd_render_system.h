#pragma once
#include <snk/common/common.h>
#include <snk/driver/rnd_driver_interface.h>

#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/render/shader/rnd_shader_manager.h>
#include <snk/render/geom/rnd_geom_manager.h>

#include <snk/render/rnd_render_pass_interface.h>
#include <snk/render/rnd_frame_context.h>
#include <functional>

namespace scn { class skybox_desc; }
namespace rnd { class skinning_manager; }
namespace rnd { struct material_compiler_interface; }
namespace res { class resource_system; }

namespace rnd
{
	using RENDER_MODE = driver::RENDER_MODE;

	class render_system
	{
	public:
		render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d, res::resource_system& res);
		~render_system() = default;
		render_system(const render_system&) = delete;
		render_system(render_system&&) = delete;
		render_system& operator= (const render_system&) = delete;
		render_system& operator= (render_system&&) = delete;
		
		const rnd::texture_manager& get_texture_manager() const { return texture_manager; }
		rnd::texture_manager& get_texture_manager() { return texture_manager; }
		const rnd::ShaderManager& get_shader_manager() const { return shader_manager; }
		rnd::ShaderManager& get_shader_manager() { return shader_manager; }
		const rnd::geom_manager& get_geom_manager() const { return geom_manager; }
		rnd::geom_manager& get_geom_manager() { return geom_manager; }

		void render_frame(frame_context& context) const;

		// Inject the render managers into the per-frame context so the scene extractor and the
		// render passes read them from frame_context instead of the rnd::get_system() singleton
		// (EPIC-49 US-49-6). Call once per frame before build_frame / render_frame.
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

	private:
		rnd::skinning_manager* m_skinning_manager = nullptr;
		std::shared_ptr<material_compiler_interface> m_material_compiler;
		std::unique_ptr<rnd::driver::driver_interface> drv = nullptr;
		rnd::texture_manager texture_manager;
		rnd::ShaderManager shader_manager;
		rnd::geom_manager geom_manager;

		std::vector<std::shared_ptr<render_pass_interface>> render_passes;
	};

	template<class T>
	void configure_pass(const frame_context& context, T& desc)
	{
		context.shaders->use(desc);
	}
}
