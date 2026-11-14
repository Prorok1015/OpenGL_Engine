#include "rnd_geom_manager.h"
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <functional>

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
		built[geom_tag] = handle;
		return (cache[geom_tag] = create_geometry(*handle)).get();
	}

	if (handle.has_error()) {
		loading.erase(geom_tag);
		egLOG("render/geometry", "Failed to load geometry desc with name {0}", geom_tag.view());
	}

	return nullptr;
}

std::size_t rnd::geom_manager::drawn_range_hash::operator()(const drawn_range& range) const noexcept
{
	std::size_t seed = std::hash<res::tag>{}(range.geometry);
	for (const std::uint32_t part : { range.count, range.base_vertex, range.base_index }) {
		seed ^= std::hash<std::uint32_t>{}(part) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
	}
	return seed;
}

bool rnd::geom_manager::draw_fits(const res::tag& geom_tag, std::uint32_t count, std::uint32_t base_vertex, std::uint32_t base_index)
{
	if (count == 0) return true;
	const auto geometry = built.find(geom_tag);
	if (geometry == built.end()) return true;

	const drawn_range range{ geom_tag, count, base_vertex, base_index };
	if (const auto known = checked_ranges.find(range); known != checked_ranges.end()) return known->second;

	const rnd::geometry_desc& desc = *geometry->second;
	const std::uint32_t stride = desc.layout.get_stride();
	const std::uint64_t vertices = stride == 0 ? 0 : desc.vertices.size() / stride;
	const std::uint64_t end = static_cast<std::uint64_t>(base_index) + count;
	bool fits = end <= desc.indices.size();
	std::uint64_t highest = 0;
	if (fits) {
		for (std::uint64_t i = base_index; i < end; ++i) {
			highest = std::max<std::uint64_t>(highest, desc.indices[static_cast<std::size_t>(i)]);
		}
		fits = static_cast<std::uint64_t>(base_vertex) + highest < vertices;
	}
	if (!fits) {
		egLOG_WARN("render/geometry", "a draw of '{}' is left out: indices {}..{} from vertex {} do not fit its {} indices and {} vertices",
			geom_tag.view(), base_index, end, base_vertex, desc.indices.size(), vertices);
	}
	checked_ranges.emplace(range, fits);
	return fits;
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