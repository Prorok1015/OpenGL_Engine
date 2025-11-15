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

rnd::ShaderManager::ShaderManager(driver::driver_interface* ptr)
	: drv(ptr)
	, _matrices(drv->create_uniform_buffer(sizeof(global_params), 0))
	, sun_light(drv->create_uniform_buffer(sizeof(lights_params), 1))
	, bones_buffer(drv->create_uniform_buffer(sizeof(bones_matrices), 2))
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
}

void rnd::ShaderManager::update_global_bones_matrices(const bones_matrices& val, std::size_t count) const
{
	bones_buffer->set_data(glm::value_ptr(val.row_height), sizeof(decltype(val.row_height)), offsetof(bones_matrices, row_height));
	bones_buffer->set_data(glm::value_ptr(val.bone_count), sizeof(decltype(val.bone_count)), offsetof(bones_matrices, bone_count));
	bones_buffer->set_data(glm::value_ptr(val.bones), sizeof(decltype(val.bones)), offsetof(bones_matrices, bones));
}
