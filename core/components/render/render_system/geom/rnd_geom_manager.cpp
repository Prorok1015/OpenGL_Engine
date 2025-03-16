#include "rnd_geom_manager.h"
#include "rnd_buffer_interface.h"
#include "res_system.h"

rnd::geom_manager::geom_manager(rnd::driver::driver_interface* _driver)
	: drv(_driver)
{
}

rnd::geom_manager::~geom_manager()
{
}

rnd::driver::vertex_array_interface* rnd::geom_manager::require_geometry(res::tag geom_tag)
{
    ASSERT_MSG(geom_tag.is_valid(), "geom tag is invalid!");

	if (auto* va = find_geometry(geom_tag)) {
		return va;
	}

    auto model = res::get_system().require_resource<res::Model>(geom_tag);
    auto va = create_geometry(model);
    return (cache[model->get_tag()] = std::move(va)).get();
}

rnd::driver::vertex_array_interface* rnd::geom_manager::find_geometry(res::tag geom_tag)
{
	auto it = cache.find(geom_tag);
	if (it != cache.end()) {
		return it->second.get();
	}

	return nullptr;
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::geom_manager::create_geometry(std::shared_ptr<res::Model> model)
{
    auto vertex_array = drv->create_vertex_array();

    std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer = drv->create_buffer();
    vertex_buffer->reserve(8000000 * sizeof(res::Vertex));
    vertex_buffer->set_layout(
        {
            {rnd::driver::SHADER_DATA_TYPE::VEC3_F, "position"},
            {rnd::driver::SHADER_DATA_TYPE::VEC3_F, "normal"},
            {rnd::driver::SHADER_DATA_TYPE::VEC2_F, "texture_position"},
            {rnd::driver::SHADER_DATA_TYPE::VEC3_F, "tangent"},
            {rnd::driver::SHADER_DATA_TYPE::VEC3_F, "bitangent"},
            {rnd::driver::SHADER_DATA_TYPE::VEC4_F, "bones_weight"},
            {rnd::driver::SHADER_DATA_TYPE::VEC4_F, "color"},
        }
        );
    vertex_buffer->set_data(model->get_model_pres().data.vertices);

    vertex_array->add_vertex_buffer(vertex_buffer);
    std::shared_ptr<rnd::driver::buffer_interface> index_buffer = drv->create_buffer();
    index_buffer->set_data(model->get_model_pres().data.indices);

    vertex_array->set_index_buffer(index_buffer);

    return vertex_array;
}