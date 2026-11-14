#pragma once
#include <snk/common/common.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_vertex_array_interface.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace res { class resource_system; }

namespace rnd
{
	class geom_manager
	{
	public:
		geom_manager(rnd::driver::driver_interface* driver, res::resource_system& res_sys);
		~geom_manager();

		rnd::driver::vertex_array_interface* require_geometry(res::tag geom_tag);
		rnd::driver::vertex_array_interface* find_geometry(res::tag geom_tag);

		// Whether a draw of `count` indices from `base_index`, each offset by `base_vertex`, stays inside the buffers of
		// this geometry. The three numbers come from a desc file or from whoever wrote the component (an agent too): a
		// draw that does not fit reads outside the buffers every frame (#927). True for a geometry not loaded here.
		bool draw_fits(const res::tag& geom_tag, std::uint32_t count, std::uint32_t base_vertex, std::uint32_t base_index);

	private:
		std::unique_ptr<driver::vertex_array_interface> create_geometry(rnd::geometry_desc& geom_desc);

	private:
		res::resource_system& m_res_sys;
		driver::driver_interface* drv;
		std::unordered_map<res::tag, std::unique_ptr<driver::vertex_array_interface>> cache;
		std::unordered_map<res::tag, res::res_handle<rnd::geometry_desc>> loading;
		std::unordered_set<res::tag> refused;

		// A range someone asked to draw, and whether it fits: looked at once, asked about every frame.
		struct drawn_range
		{
			res::tag geometry;
			std::uint32_t count = 0;
			std::uint32_t base_vertex = 0;
			std::uint32_t base_index = 0;

			bool operator==(const drawn_range&) const = default;
		};
		struct drawn_range_hash
		{
			std::size_t operator()(const drawn_range& range) const noexcept;
		};
		// The loaded desc of each geometry built here: its indices are what a range is checked against.
		std::unordered_map<res::tag, res::res_handle<rnd::geometry_desc>> built;
		std::unordered_map<drawn_range, bool, drawn_range_hash> checked_ranges;
	};
}