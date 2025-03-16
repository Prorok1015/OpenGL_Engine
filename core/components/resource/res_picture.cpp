#include "res_picture.h"
#include "res_system.h"
#include "image_loader.h"

res::Picture::Picture(const res::tag& tag)
	: res::Resource(tag)
{
	std::string path = resource_system::get_absolut_path(tag);
	if (path.empty()) {
		return;
	}

	if (std::unique_ptr<stb_image::Image> img = load_image(path.data())) {
		size_.x = img->width();
		size_.y = img->height();
		channels_ = img->channels_count();
		data_ = img->pop_data();
	}
}

res::Picture::Picture(const tag& tag, glm::ivec2 size_1, int channels_1, unsigned char* data_1)
	: res::Resource(tag)
	, is_embedded_picture(true)
	, size_(size_1)
	, channels_(channels_1)
	, data_(data_1)
{

}

res::Picture::~Picture()
{
	if (!is_embedded_picture) {
		stb_image::Image::free_image_data(data_);
	}
	else {
		delete[] data_;
	}
}
