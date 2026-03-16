#pragma once
#include <common.h>
#include <rnd_driver_interface.h>

#include "texture/rnd_texture_manager.h"
#include "shader/rnd_shader_manager.h"
#include "geom/rnd_geom_manager.h"

#include "rnd_renderer_base.h"

#include "rnd_render_pass_interface.h"
#include "rnd_frame_context.h"
#include <functional>

namespace scn { class skybox_desc; }
namespace rnd { class skinning_manager; }

namespace rnd
{
	using RENDER_MODE = driver::RENDER_MODE;

	class render_system
	{
	public:
		render_system(std::unique_ptr<rnd::driver::driver_interface> driver, desc::desc_system& d);
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

		void activate_renderer(std::weak_ptr<renderer_base> renderer);
		void deactivate_renderer(std::weak_ptr<renderer_base> renderer);

		void render() const;
		void render_frame(frame_context& context) const;

		void add_render_pass(std::shared_ptr<render_pass_interface> pass) {
			render_passes.push_back(pass);
		}

		void remove_render_pass(std::shared_ptr<render_pass_interface> pass) {
			render_passes.erase(std::remove(render_passes.begin(), render_passes.end(), pass));
		}

		RENDER_MODE get_render_mode() const { return render_mode; }
		void set_render_mode(RENDER_MODE val) { render_mode = val; }

		rnd::driver::driver_interface* get_driver() const { return drv.get(); }

		void set_skinning_manager(rnd::skinning_manager* sm) { m_skinning_manager = sm; }
		rnd::skinning_manager* get_skinning_manager() const { return m_skinning_manager; }

	private:
		rnd::skinning_manager* m_skinning_manager = nullptr;
		std::unique_ptr<rnd::driver::driver_interface> drv = nullptr;
		rnd::texture_manager texture_manager;
		rnd::ShaderManager shader_manager;
		rnd::geom_manager geom_manager;

		std::vector<std::weak_ptr<renderer_base>> renderers_list;

		std::vector<std::shared_ptr<render_pass_interface>> render_passes;

		RENDER_MODE render_mode = RENDER_MODE::TRIANGLE;
	};

	render_system& get_system();

	struct render_mode_component {
		RENDER_MODE mode;
	};


	template<class T>
	void configure_pass(T& desc)
	{
		rnd::get_system().get_shader_manager().use(desc);
	}
}
