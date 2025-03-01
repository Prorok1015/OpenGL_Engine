#pragma once
#include "common.h"
#include "rnd_driver_interface.h"
#include "rnd_vertex_array_interface.h"
#include "res_tag.h"
#include "res_resource_model.h"

namespace rnd
{
	class geom_manager
	{
	public:
		geom_manager(rnd::driver::driver_interface* driver);
		~geom_manager();

		rnd::driver::vertex_array_interface* require_geometry(res::Tag geom_tag);
		rnd::driver::vertex_array_interface* find_geometry(res::Tag geom_tag);

	private:
		std::unique_ptr<driver::vertex_array_interface> create_geometry(std::shared_ptr<res::Model> model);

	private:
		driver::driver_interface* drv;
		std::unordered_map<res::Tag, std::unique_ptr<driver::vertex_array_interface>> cache;
	};
}