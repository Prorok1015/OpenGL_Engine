#include "rnd_shader_manager.h"
#include "res_system.h"
#include "resources/res_resource_text.h"


namespace glm
{
	static decltype(auto) value_ptr(auto&& v)
	{
		return std::addressof(v);
	}
}

rnd::ShaderManager::ShaderManager(driver::driver_interface* ptr, res::resource_system& res_sys)
	: drv(ptr)
	, m_res_sys(res_sys)
	, _matrices(drv->create_uniform_buffer(sizeof(global_params), 0))
	, sun_light(drv->create_uniform_buffer(sizeof(lights_params), 1))
{
}

rnd::ShaderManager::~ShaderManager()
{
}

void rnd::ShaderManager::update_global_uniform(const global_params& val) const
{
	_matrices->set_data(glm::value_ptr(val.projection), sizeof(decltype(val.projection)), offsetof(global_params, projection));
	_matrices->set_data(glm::value_ptr(val.view), sizeof(decltype(val.view)), offsetof(global_params, view));
	_matrices->set_data(glm::value_ptr(val.time), sizeof(decltype(val.time)), offsetof(global_params, time));
	_matrices->set_data(glm::value_ptr(val.view_position), sizeof(decltype(val.view_position)), offsetof(global_params, view_position));
}

void rnd::ShaderManager::update_global_sun(const lights_params& val) const
{
	sun_light->set_data(glm::value_ptr(val.directional_lights), sizeof(decltype(val.directional_lights)), offsetof(lights_params, directional_lights));
	sun_light->set_data(glm::value_ptr(val.point_lights), sizeof(decltype(val.point_lights)), offsetof(lights_params, point_lights));
	sun_light->set_data(&val.dir_light_count, sizeof(val.dir_light_count) + sizeof(val.point_light_count), offsetof(lights_params, dir_light_count));
}
