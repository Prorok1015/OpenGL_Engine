#pragma once
#include "common.h"
#include "rnd_driver_interface.h"
#include "rnd_vertex_array_interface.h"
#include "res_tag.h"
#include "desc_system.h"
#include "rnd_geometry_desc.h"
#include <unordered_set>

namespace res { class resource_system; }

namespace rnd
{
	class geom_manager
	{
	public:
		geom_manager(rnd::driver::driver_interface* driver, desc::desc_system& d, res::resource_system& res_sys);
		~geom_manager();

		rnd::driver::vertex_array_interface* require_geometry(res::tag geom_tag);
		rnd::driver::vertex_array_interface* find_geometry(res::tag geom_tag);

	private:
		std::unique_ptr<driver::vertex_array_interface> create_geometry(rnd::geometry_desc& geom_desc);

	private:
		desc::desc_system& desc_system;
		res::resource_system& m_res_sys;
		driver::driver_interface* drv;
		std::unordered_map<res::tag, std::unique_ptr<driver::vertex_array_interface>> cache;
		std::unordered_map<res::tag, res::res_handle<rnd::geometry_desc>> loading;
	};
}