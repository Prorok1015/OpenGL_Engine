#pragma once
#include <snk/common/common.h>
#include <snk/driver/rnd_driver_interface.h>
#include <snk/driver/rnd_vertex_array_interface.h>
#include <snk/resource/res_tag.h>
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/common/snk_api.h>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace res { class resource_system; }

namespace rnd
{
	// How many answers of geom_manager::draw_fits one geometry keeps; a range past them is checked again at each draw.
	inline constexpr std::size_t k_checked_ranges_cap = 256;

	// Exported for the tests only: internal to the renderer.
	class SNK_API geom_manager
	{
	public:
		geom_manager(rnd::driver::driver_interface* driver, res::resource_system& res_sys);
		~geom_manager();

		geom_manager(const geom_manager&) = delete;
		geom_manager(geom_manager&&) = delete;
		geom_manager& operator=(const geom_manager&) = delete;
		geom_manager& operator=(geom_manager&&) = delete;

		rnd::driver::vertex_array_interface* require_geometry(res::tag geom_tag);
		rnd::driver::vertex_array_interface* find_geometry(res::tag geom_tag);

		// Whether a draw of `count` indices from `base_index`, each offset by `base_vertex`, stays inside the buffers of
		// this geometry. The three numbers come from a desc file or from whoever wrote the component (an agent too): a
		// draw that does not fit reads outside the buffers every frame (#927). True for a geometry not loaded here.
		// A range is looked at once and its answer kept with the geometry; one warning per geometry.
		bool draw_fits(const res::tag& geom_tag, std::uint32_t count, std::uint32_t base_vertex, std::uint32_t base_index);

	private:
		struct checked_range
		{
			std::uint32_t base_index = 0;
			std::uint32_t count = 0;
			std::uint32_t base_vertex = 0;

			bool operator==(const checked_range&) const = default;
		};
		struct checked_range_hash
		{
			std::size_t operator()(const checked_range& range) const noexcept
			{
				std::uint64_t hash = 0xcbf29ce484222325ull;
				for (const std::uint32_t part : { range.base_index, range.count, range.base_vertex }) {
					hash = (hash ^ part) * 0x100000001b3ull;
				}
				return static_cast<std::size_t>(hash);
			}
		};
		// The answers live in the record of their geometry, so they go when it does.
		struct built_geometry
		{
			std::unique_ptr<driver::vertex_array_interface> vertex_array;
			// A copy, not the desc: the vertices, the bounds and the tree are not held for the check.
			std::vector<std::uint32_t> indices;
			std::uint64_t vertex_count = 0;
			std::unordered_map<checked_range, bool, checked_range_hash> checked;
			bool warned = false;
		};

		std::unique_ptr<driver::vertex_array_interface> create_geometry(rnd::geometry_desc& geom_desc);

	private:
		res::resource_system& m_res_sys;
		driver::driver_interface* drv;
		std::unordered_map<res::tag, built_geometry> cache;
		std::unordered_map<res::tag, res::res_handle<rnd::geometry_desc>> loading;
		std::unordered_set<res::tag> refused;
	};
}