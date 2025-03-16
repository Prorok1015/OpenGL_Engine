#include "rnd_texture.h"
#include "res_system.h"
#include "res_picture.h"

std::unique_ptr<rnd::driver::texture_interface> rnd::Texture::load(driver::driver_interface* drv, const res::tag& tag)
{
	auto res = res::get_system().require_resource2<res::Picture>(tag);
	if (!res) {
		return nullptr;
	}

    driver::texture_header header;
    header.data.initial_data = res->data();
    switch (res->channels())
    {
    case 1: header.data.format = driver::texture_header::TYPE::R8; break;
    case 3: header.data.format = driver::texture_header::TYPE::RGB8; break;
    case 4: header.data.format = driver::texture_header::TYPE::RGBA8; break;
    default:
        break;
    }
    header.data.extent.width = res->size().x;
    header.data.extent.height = res->size().y;
    header.wrap = driver::texture_header::WRAPPING::REPEAT;
    header.min = driver::texture_header::FILTERING::LINEAR_MIPMAP;
    header.mag = driver::texture_header::FILTERING::LINEAR;

	return drv->create_texture(header);
}