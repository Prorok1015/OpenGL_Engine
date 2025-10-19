#include "res_resource_picture.h"

res::picture_resource::picture_resource(const tag& tag, glm::ivec2 size_1, int channels_1, unsigned char* data_1)
	: res::resource_entry(tag)
	, is_embedded_picture(true)
	, size_(size_1)
	, channels_(channels_1)
	, data_(data_1)
{

}

res::picture_resource::~picture_resource()
{
	delete[] data_;
}
