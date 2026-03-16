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

	auto it = loading.find(geom_tag); 
	res::res_handle<rnd::geometry_desc> handle;
	if (it == loading.end()) {
		handle = res::get_system().require<rnd::geometry_desc>(geom_tag);
		loading[geom_tag] = handle;
	} else {
		handle = it->second;
	}

	if (handle.is_ready()) {
		loading.erase(geom_tag);
		return (cache[geom_tag] = create_geometry(*handle)).get();
	}

	if (handle.has_error()) {
		loading.erase(geom_tag);
		egLOG("render/geometry", "Failed to load geometry desc with name {0}", geom_tag.view());
	}

	return nullptr;
}

rnd::driver::vertex_array_interface* rnd::geom_manager::find_geometry(res::tag geom_tag)
{
	auto it = cache.find(geom_tag);
	if (it != cache.end()) {
		return it->second.get();
	}

	return nullptr;
}

std::unique_ptr<rnd::driver::vertex_array_interface> rnd::geom_manager::create_geometry(rnd::geometry_desc& geom_desc)
{
	auto vertex_array = drv->create_vertex_array();
	std::shared_ptr<rnd::driver::buffer_interface> vertex_buffer = drv->create_buffer();
	vertex_buffer->set_layout(geom_desc.layout);
	vertex_buffer->set_data(geom_desc.vertices);
	vertex_array->add_vertex_buffer(vertex_buffer);
	std::shared_ptr<rnd::driver::buffer_interface> index_buffer = drv->create_buffer();
	index_buffer->set_data(geom_desc.indices);
	vertex_array->set_index_buffer(index_buffer);
	return vertex_array;
}