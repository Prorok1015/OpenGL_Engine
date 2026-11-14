#include "rnd_geom_manager.h"
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cstddef>

rnd::geom_manager::geom_manager(rnd::driver::driver_interface* _driver, res::resource_system& res_sys)
	: m_res_sys(res_sys)
	, drv(_driver)
{
}

rnd::geom_manager::~geom_manager()
{
}

rnd::driver::vertex_array_interface* rnd::geom_manager::require_geometry(res::tag geom_tag)
{
    ASSERT_MSG(geom_tag.is_valid(), "geom tag is invalid!");

	// The tag comes from a component, which anything may have written (#697): the rule of what data may load holds here too.
	if (std::string why = res::reference_problem(geom_tag); !why.empty()) {
		if (refused.insert(geom_tag).second) {
			egLOG_WARN("render/geometry", "{}", why);
		}
		return nullptr;
	}

	if (auto* va = find_geometry(geom_tag)) {
		return va;
	}

	auto it = loading.find(geom_tag); 
	res::res_handle<rnd::geometry_desc> handle;
	if (it == loading.end()) {
		handle = m_res_sys.require<rnd::geometry_desc>(geom_tag);
		loading[geom_tag] = handle;
	} else {
		handle = it->second;
	}

	if (handle.is_ready()) {
		loading.erase(geom_tag);
		rnd::geometry_desc& desc = *handle;
		built_geometry& geometry = cache[geom_tag];
		geometry.vertex_array = create_geometry(desc);
		geometry.indices.assign(desc.indices.begin(), desc.indices.end());
		const std::uint32_t stride = desc.layout.get_stride();
		geometry.vertex_count = stride == 0 ? std::uint64_t{ 0 } : std::uint64_t{ desc.vertices.size() / stride };
		return geometry.vertex_array.get();
	}

	if (handle.has_error()) {
		loading.erase(geom_tag);
		egLOG("render/geometry", "Failed to load geometry desc with name {0}", geom_tag.view());
	}

	return nullptr;
}

bool rnd::geom_manager::draw_fits(const res::tag& geom_tag, std::uint32_t count, std::uint32_t base_vertex, std::uint32_t base_index)
{
	if (count == 0) return true;
	const auto found = cache.find(geom_tag);
	if (found == cache.end()) return true;
	built_geometry& geometry = found->second;

	const checked_range range{ base_index, count, base_vertex };
	if (const auto known = geometry.checked.find(range); known != geometry.checked.end()) return known->second;

	const std::uint64_t end = static_cast<std::uint64_t>(base_index) + count;
	bool fits = end <= geometry.indices.size();
	if (fits) {
		const auto first = geometry.indices.begin() + static_cast<std::ptrdiff_t>(base_index);
		const std::uint32_t highest = *std::max_element(first, first + static_cast<std::ptrdiff_t>(count));
		fits = static_cast<std::uint64_t>(base_vertex) + highest < geometry.vertex_count;
	}
	if (!fits && !geometry.warned) {
		geometry.warned = true;
		egLOG_WARN("render/geometry", "'{}': draws outside its {} indices and {} vertices are left out; the first is indices {}..{} from vertex {}",
			geom_tag.view(), geometry.indices.size(), geometry.vertex_count, base_index, end, base_vertex);
	}
	// Past the cap a range is checked at every draw rather than answered wrongly.
	if (geometry.checked.size() < k_checked_ranges_cap) {
		geometry.checked.emplace(range, fits);
	}
	return fits;
}

rnd::driver::vertex_array_interface* rnd::geom_manager::find_geometry(res::tag geom_tag)
{
	auto it = cache.find(geom_tag);
	if (it != cache.end()) {
		return it->second.vertex_array.get();
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