#include "rnd_geom_manager.h"
#include <snk/driver/rnd_buffer_interface.h>
#include <snk/resource/res_system.h>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

rnd::geom_manager::geom_manager(rnd::driver::driver_interface* _driver, res::resource_system& res_sys)
	: m_res_sys(res_sys)
	, drv(_driver)
{
}

rnd::geom_manager::~geom_manager()
{
	ASSERT_MSG(on_owner_thread(), "geom_manager destruction: main thread only");
	for (const res::tag& tag : watched) {
		m_res_sys.unwatch(tag, this);
	}
}

void rnd::geom_manager::watch(const res::tag& geom_tag)
{
	if (watched.insert(geom_tag).second) {
		m_res_sys.watch(geom_tag, this, [this](const res::tag& changed) { on_changed(changed); });
	}
}

void rnd::geom_manager::unwatch(const res::tag& geom_tag)
{
	if (watched.erase(geom_tag) != 0) {
		m_res_sys.unwatch(geom_tag, this);
	}
}

rnd::driver::vertex_array_interface* rnd::geom_manager::require_geometry(res::tag geom_tag)
{
	ASSERT_MSG(geom_tag.is_valid(), "geom tag is invalid!");
	ASSERT_MSG(on_owner_thread(), "geom_manager::require_geometry: main thread only");

	// The tag comes from a component, which anything may have written (#697): the rule of what data may load holds here too.
	if (std::string why = res::reference_problem(geom_tag); !why.empty()) {
		if (refused.insert(geom_tag).second) {
			egLOG_WARN("render/geometry", "{}", why);
		}
		return nullptr;
	}

	const auto pending = loading.find(geom_tag);
	if (const auto cached = cache.find(geom_tag); cached != cache.end()) {
		cached->second.last_used = m_frame;
		if (pending == loading.end()) {
			return cached->second.vertex_array.get();
		}
		// Its desc changed: the previous vertex array is drawn until the new one is built.
		if (pending->second.is_ready()) {
			res::res_handle<rnd::geometry_desc> changed = pending->second;
			loading.erase(pending);
			if (auto rebuilt = build(*changed)) {
				rebuilt->last_used = m_frame;
				cached->second = std::move(*rebuilt);
			} else {
				egLOG_WARN("render/geometry", "'{}' changed but gives no vertex array: the previous geometry stays",
					geom_tag.view());
			}
		} else if (pending->second.has_error()) {
			const std::string why = pending->second.error_text();
			loading.erase(pending);
			egLOG_WARN("render/geometry", "'{}' changed but failed to load ({}): the previous geometry stays until it "
				"changes again", geom_tag.view(), why);
		}
		return cached->second.vertex_array.get();
	}

	res::res_handle<rnd::geometry_desc> handle;
	if (pending == loading.end()) {
		handle = m_res_sys.require<rnd::geometry_desc>(geom_tag);
		loading[geom_tag] = handle;
		watch(geom_tag);
	} else {
		handle = pending->second;
	}

	if (handle.is_ready()) {
		loading.erase(geom_tag);
		auto geometry = build(*handle);
		if (!geometry) {
			return nullptr;
		}
		geometry->last_used = m_frame;
		rnd::driver::vertex_array_interface* const built = geometry->vertex_array.get();
		cache.insert_or_assign(geom_tag, std::move(*geometry));
		return built;
	}

	if (handle.has_error()) {
		loading.erase(geom_tag);
		unwatch(geom_tag);
		egLOG("render/geometry", "Failed to load geometry desc with name {0}", geom_tag.view());
	}

	return nullptr;
}

std::optional<rnd::geom_manager::built_geometry> rnd::geom_manager::build(rnd::geometry_desc& desc)
{
	// Built aside and moved in last: a failure or an exception leaves no half-made record in the cache.
	built_geometry geometry;
	geometry.vertex_array = create_geometry(desc);
	if (!geometry.vertex_array) {
		return std::nullopt;
	}
	geometry.indices.assign(desc.indices.begin(), desc.indices.end());
	const std::uint32_t stride = desc.layout.get_stride();
	geometry.vertex_count = stride == 0 ? std::uint64_t{ 0 } : std::uint64_t{ desc.vertices.size() / stride };
	return geometry;
}

void rnd::geom_manager::on_changed(const res::tag& geom_tag)
{
	ASSERT_MSG(on_owner_thread(), "geom_manager::on_changed: main thread only");
	if (!cache.contains(geom_tag) && !loading.contains(geom_tag)) {
		return;
	}
	// The change evicted the block from the resource cache: this reads the new data, once.
	loading.insert_or_assign(geom_tag, m_res_sys.require<rnd::geometry_desc>(geom_tag));
}

void rnd::geom_manager::begin_frame()
{
	ASSERT_MSG(on_owner_thread(), "geom_manager::begin_frame: main thread only");
	++m_frame;
	for (auto it = cache.begin(); it != cache.end();) {
		// Also one whose reload is in flight: a geometry nobody draws is not built again.
		if (it->second.last_used + k_unused_frames_kept < m_frame) {
			loading.erase(it->first);
			unwatch(it->first);
			it = cache.erase(it);
		} else {
			++it;
		}
	}
}

bool rnd::geom_manager::draw_fits(const res::tag& geom_tag, std::uint32_t count, std::uint32_t base_vertex, std::uint32_t base_index)
{
	if (count == 0) return true;
	const auto found = cache.find(geom_tag);
	if (found == cache.end()) return true;
	built_geometry& geometry = found->second;
	geometry.last_used = m_frame;

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