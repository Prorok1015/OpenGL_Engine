#include "rnd_texture_manager.h"
#include "res_system.h"
#include "resources/res_resource_picture.h"
#include "rnd_texture_desc.h"

rnd::driver::texture_interface* rnd::texture_manager::require_texture(const res::tag& tag)
{
	if (auto it = cache.find(tag); it != cache.end()) {
		auto& texture = it->second;
		return texture.get();
	}

	auto it = loading.find(tag);
	res::res_handle<rnd::texture_desc> handle;
	if (it == loading.end()) {
		handle = res::get_system().require<rnd::texture_desc>(tag);
		loading[tag] = handle;
	} else {
		handle = it->second;
	}

	if (handle.has_error()) {
		loading.erase(tag);
		egLOG("render/geometry", "Failed to load texture desc with name {0}", tag.view());
	}

	if (!handle.is_ready()) {
		return nullptr;
	}

	loading.erase(tag);

	auto loader_it = loaders.find(std::string{ handle->get_type() });
	if (loaders.end() != loader_it) {
		auto& texture = cache[tag] = loader_it->second(drv, *handle);
		return texture.get();
	}

	return nullptr;
}

rnd::driver::texture_interface* rnd::texture_manager::generate_texture(const res::tag& tag, rnd::driver::texture_header header)
{
	auto it = cache.find(tag);
	if (it != cache.end()) {
		auto& texture = it->second;
		return texture.get();
	}

	auto& texture = cache[tag] = drv->create_texture(header);
    return texture.get();
}
 
rnd::driver::texture_interface* rnd::texture_manager::generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data)
{
	rnd::driver::texture_header header;
	header.data.extent.width = size.x;
	header.data.extent.height = size.y;
	header.data.format = channels;
	header.data.initial_data = data.data();
	header.data.mip_levels = 1;	

	return generate_texture(tag, header);
}

rnd::driver::texture_interface* rnd::texture_manager::find(const res::tag& tag) const
{
	auto it = cache.find(tag);
	if (it != cache.end()) {
		auto& texture = it->second;
		return texture.get();
	}

	return nullptr;
}

void rnd::texture_manager::remove(const res::tag& tag)
{
	cache.erase(tag);
}

void rnd::texture_manager::clear_cache()
{ 
	std::vector<res::tag> list_to_delete;
	for (auto& [key, _] : cache)
	{
		if (key.protocol() != res::tag::memory) {
			list_to_delete.push_back(key);
		}
	}

	for (auto& key : list_to_delete) {
		cache.erase(key);
	}
}