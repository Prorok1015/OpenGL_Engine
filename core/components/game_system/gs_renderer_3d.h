#pragma once
#include <rnd_renderer_base.h>
#include <scn_model.h>
#include <camera/Camera.h>

namespace rnd::driver
{
	class driver_interface;
	class vertex_array_interface;
	class buffer_interface;
}

namespace gs
{
	class renderer_3d : public rnd::renderer_base
	{
	public:
		renderer_3d();
		virtual ~renderer_3d() override {}

		virtual void on_render(rnd::driver::driver_interface* drv);

		void draw(scene::Model& val, rnd::driver::driver_interface* drv);
		void draw(scene::Mesh& mesh, rnd::driver::driver_interface* drv);

	public:
		camera_static* camera = nullptr;
		std::vector<scene::Model> scene_objects;
		std::shared_ptr<rnd::driver::vertex_array_interface> vertex_array;
		std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer;
		std::shared_ptr<rnd::driver::buffer_interface> index_buffer;
	};
}