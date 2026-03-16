#include "res_resource_picture.h"

res::picture_resource::picture_resource(const tag& tag, glm::ivec2 size_1, int channels_1, unsigned char* data_1)
	: res::resource_entry(tag)
	, is_embedded_picture(true)
	, size_(size_1)
	, channels_(channels_1)
	, data_(data_1)
{

}

res::picture_resource::picture_resource(const picture_resource& other)
	: res::resource_entry(other.m_tag)
	, size_(other.size_)
	, channels_(other.channels_)
{
	const std::size_t data_size = size_.x * size_.y * channels_;
	data_ = new unsigned char[data_size];
	std::memcpy(data_, other.data_, data_size);
}

res::picture_resource& res::picture_resource::operator= (const picture_resource& other)
{
	if (this != &other) {
		m_tag = other.m_tag;
		size_ = other.size_;
		channels_ = other.channels_;
		const std::size_t data_size = size_.x * size_.y * channels_;
		delete[] data_;
		data_ = new unsigned char[data_size];
		std::memcpy(data_, other.data_, data_size);
	}
	return *this;
}

res::picture_resource::picture_resource(picture_resource&& other) noexcept
	: res::resource_entry(other.m_tag)
	, size_(other.size_)
	, channels_(other.channels_)
	, data_(other.data_)
{
	other.data_ = nullptr;
	other.size_ = glm::ivec2{ 0,0 };
	other.channels_ = 0;
}

res::picture_resource& res::picture_resource::operator= (picture_resource&& other) noexcept
{
	if (this != &other) {
		m_tag = other.m_tag;
		size_ = other.size_;
		channels_ = other.channels_;
		delete[] data_;
		data_ = other.data_;
		other.data_ = nullptr;
		other.size_ = glm::ivec2{ 0,0 };
		other.channels_ = 0;
	}
	return *this;
}

res::picture_resource::~picture_resource()
{
	delete[] data_;
}
