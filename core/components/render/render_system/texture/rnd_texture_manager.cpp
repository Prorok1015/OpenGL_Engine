#include "rnd_texture_manager.h"
#include "res_system.h"
#include "res_picture.h"
#include "rnd_texture_desc.h"

rnd::driver::texture_interface* rnd::TextureManager::require_texture(const res::tag& tag)
{
	auto it = cache.find(tag);
	if (it != cache.end()) {
		auto& texture = it->second;
		return texture.get();
	}

	auto desc = desc_system.get_desc<rnd::texture_desc>(tag);
	if (!desc) {
		ASSERT_FAIL("texture desc not found");
		return nullptr;
	}

	ASSERT_MSG(desc->is_loaded(), "desc hasn't loaded yet");
	if (!desc->is_loaded()) {
		return nullptr;
	}

    auto res = res::get_system().require_resource<res::Picture>(desc->txm_tag);
    if (!res) {
        return nullptr;
    }

    driver::texture_header header = desc->header;
    header.data.initial_data = res->data();


	switch (res->channels())
	{
	case 1: { 
		if (header.data.format != driver::texture_header::TYPE::R8) {
			header.data.format = driver::texture_header::TYPE::R8;
		}
	}break;
	case 3: {
		if (header.data.format != driver::texture_header::TYPE::RGB8) {
			header.data.format = driver::texture_header::TYPE::RGB8;
		}
	}break;
	case 4: {
		if (header.data.format != driver::texture_header::TYPE::RGBA8) {
			header.data.format = driver::texture_header::TYPE::RGBA8;
		}
	}break;
	default:
		break;
	}

    if (header.data.extent.width != res->size().x) {
		header.data.extent.width = res->size().x;
    }
    if (header.data.extent.height != res->size().y) {
        header.data.extent.height = res->size().y;
    }
    //ASSERT_MSG(header.data.extent.width == res->size().x, "Texture's width doesn't equal");
    //ASSERT_MSG(header.data.extent.height == res->size().y, "Texture's height doesn't equal");

	auto& texture = cache[tag] = drv->create_texture(header);
	return texture.get();
}

rnd::driver::texture_interface* rnd::TextureManager::require_cubemap_texture(const std::vector<res::tag>& tags)
{
	auto it = cache.find(tags.front());
    if (it != cache.end()) {
        return it->second.get();
    }

    rnd::driver::texture_header cb_header;

    auto load_face_data = [](res::tag tag) -> std::tuple<std::vector<unsigned char>, rnd::driver::texture_header::TYPE, uint32_t, uint32_t> {
        auto res = res::get_system().require_resource<res::Picture>(tag);
        rnd::driver::texture_header::TYPE format;
        switch (res->channels()) {
            case 1: format = rnd::driver::texture_header::TYPE::R8; break;
            case 3: format = rnd::driver::texture_header::TYPE::RGB8; break;
            case 4: format = rnd::driver::texture_header::TYPE::RGBA8; break;
            default:
                // Handle unsupported channel counts appropriately (e.g., throw an exception)
                throw std::runtime_error("Unsupported number of channels in cubemap texture");
        }

		std::vector<unsigned char> data(res->size().x * res->size().y * res->channels());
		std::copy(res->data(), res->data() + data.size(), data.begin());

        return std::make_tuple(std::move(data), format, res->size().x, res->size().y);
    };

    // 1. Create Texture (Empty) & allocate storage
    auto [right_data, right_format, right_width, right_height] = load_face_data(tags[0]);
    cb_header.data.extent.width = right_width;
    cb_header.data.extent.height = right_height;
    cb_header.data.format = right_format;
    cb_header.data.extent.faces = 6;  // Important: 6 faces for cubemap

    cb_header.wrap = rnd::driver::texture_header::WRAPPING::CLAMP_TO_EDGE;
    cb_header.min = rnd::driver::texture_header::FILTERING::LINEAR;
    cb_header.mag = rnd::driver::texture_header::FILTERING::LINEAR;
    cb_header.type = rnd::driver::TEXTURE_TYPE::TEXTURE_CUBE_MAP;
    cb_header.usage = rnd::driver::TEXTURE_USAGE::SAMPLED;
    cb_header.data.mip_levels = 1;

    auto& texture = cache[tags.front()] = drv->create_texture(cb_header);

    // 2. Update Data for each face
    auto update_face = [&](rnd::driver::texture_interface* texture_intf, res::tag tag, uint32_t face) {
        auto [data, format, width, height] = load_face_data(tag);
        texture_intf->update_data(data.data(), 0, 0, 0, width, height, 1, 0, 0, face); // face parameter is crucial here!
    };


    update_face(texture.get(), tags[0], 0); // Right
    update_face(texture.get(), tags[1], 1); // Left
    update_face(texture.get(), tags[2], 2); // Bottom
    update_face(texture.get(), tags[3], 3); // Top
    update_face(texture.get(), tags[4], 4); // Front
    update_face(texture.get(), tags[5], 5); // Back

    return texture.get();
}

rnd::driver::texture_interface* rnd::TextureManager::generate_texture(const res::tag& tag, rnd::driver::texture_header header)
{
	auto it = cache.find(tag);
	if (it != cache.end()) {
		auto& texture = it->second;
		return texture.get();
	}

	auto& texture = cache[tag] = drv->create_texture(header);
    return texture.get();
}
 
rnd::driver::texture_interface* rnd::TextureManager::generate_texture(const res::tag& tag, glm::ivec2 size, rnd::driver::texture_header::TYPE channels, std::vector<unsigned char> data)
{
	rnd::driver::texture_header header;
	header.data.extent.width = size.x;
	header.data.extent.height = size.y;
	header.data.format = channels;
	header.data.initial_data = data.data();
	header.data.mip_levels = 1;	

	return generate_texture(tag, header);
}

rnd::driver::texture_interface* rnd::TextureManager::find(const res::tag& tag) const
{
	auto it = cache.find(tag);
	if (it != cache.end()) {
		auto& texture = it->second;
		return texture.get();
	}

	return nullptr;
}

void rnd::TextureManager::remove(const res::tag& tag)
{
	cache.erase(tag);
}

void rnd::TextureManager::clear_cache()
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