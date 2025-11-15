#include "rnd_geom_manager.h"
#include "rnd_buffer_interface.h"
#include "res_system.h"

rnd::geom_manager::geom_manager(rnd::driver::driver_interface* _driver, desc::desc_system& d)
	: drv(_driver)
	, desc_system(d)
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

	auto desc = desc_system.get_desc<rnd::geometry_desc>(geom_tag);
	if (!desc) {
		ASSERT_FAIL("geometry desc not found");
		return nullptr;
	}

	ASSERT_MSG(desc->is_loaded(), "desc hasn't loaded yet");
	if (!desc->is_loaded()) {
		return nullptr;
	}

    auto va = create_geometry(desc);
    return (cache[geom_tag] = std::move(va)).get();
}

rnd::driver::vertex_array_interface* rnd::geom_manager::find_geometry(res::tag geom_tag)
{
	auto it = cache.find(geom_tag);
	if (it != cache.end()) {
		return it->second.get();
	}

	return nullptr;
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::geom_manager::create_geometry(std::shared_ptr<rnd::geometry_desc> geom_desc)
{
	auto vertex_array = drv->create_vertex_array();
	std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer = drv->create_buffer();
	vertex_buffer->set_layout(geom_desc->layout);
	vertex_buffer->set_data(geom_desc->vertices);
	vertex_array->add_vertex_buffer(vertex_buffer);
	std::shared_ptr<rnd::driver::buffer_interface> index_buffer = drv->create_buffer();
	index_buffer->set_data(geom_desc->indices);
	vertex_array->set_index_buffer(index_buffer);
	return vertex_array;
}