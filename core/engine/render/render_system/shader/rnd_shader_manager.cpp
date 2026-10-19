#include <snk/render/shader/rnd_shader_manager.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>


rnd::ShaderManager::ShaderManager(driver::driver_interface* ptr, res::resource_system& res_sys)
	: drv(ptr)
	, m_res_sys(res_sys)
{
	m_layouts.frame = drv->create_bind_group_layout(frame_layout_desc());
	m_layouts.composition = drv->create_bind_group_layout(composition_layout_desc());
	m_layouts.sky = drv->create_bind_group_layout(sky_layout_desc());
	m_layouts.object = drv->create_bind_group_layout(object_layout_desc());

	driver::buffer_desc matrices;
	matrices.size = sizeof(global_params);
	matrices.usage = driver::buffer_usage::uniform;
	matrices.update = driver::buffer_update::dynamic;
	matrices.debug_name = "Matrices";
	m_matrices = drv->create_buffer(matrices);

	driver::buffer_desc lights;
	lights.size = sizeof(lights_params);
	lights.usage = driver::buffer_usage::uniform;
	lights.update = driver::buffer_update::dynamic;
	lights.debug_name = "GlobalLights";
	m_lights = drv->create_buffer(lights);

	driver::bind_group_desc frame;
	frame.layout = m_layouts.frame;
	frame.entries.push_back({ driver::binding_type::ubo, bindings::matrices_slot, m_matrices, 0, sizeof(global_params), nullptr });
	frame.entries.push_back({ driver::binding_type::ubo, bindings::lights_slot, m_lights, 0, sizeof(lights_params), nullptr });
	m_frame_group = drv->create_bind_group(frame, driver::lifetime::persistent);
}

rnd::ShaderManager::~ShaderManager()
{
	drv->destroy_bind_group(m_frame_group);
	drv->destroy_buffer(m_matrices);
	drv->destroy_buffer(m_lights);
}

void rnd::ShaderManager::update_global_uniform(const global_params& val) const
{
	drv->update_buffer(m_matrices, 0, std::as_bytes(std::span{ &val, 1 }));
}

void rnd::ShaderManager::update_global_sun(const lights_params& val) const
{
	drv->update_buffer(m_lights, 0, std::as_bytes(std::span{ &val, 1 }));
}

void rnd::ShaderManager::bind_frame() const
{
	drv->set_bind_group(driver::frame_set, m_frame_group);
}
