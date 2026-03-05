#include "image.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <iostream>
#include <string>
#include <stdexcept>
#include <array>
#include <cstring>


stb_image::Image::Image(const std::string_view filename)
{
	if (!read(filename)) {
		throw std::runtime_error("File read faild"); //Loger(L"  ", __func__, __LINE__, __FILEW__);
	}			
}

stb_image::Image::Image(Image&& rhs) noexcept
{
	*this = std::move(rhs);
}

stb_image::Image& stb_image::Image::operator=(stb_image::Image&& rhs) noexcept
{
	std::swap(data_, rhs.data_);
	width_ = rhs.width_;
	height_ = rhs.height_;
	channels_ = rhs.channels_;
	size_ = rhs.size_;
	flipFlag_ = rhs.flipFlag_;
	return *this;
}

stb_image::Image::~Image()
{
	free_image_data(data_);
}

void stb_image::Image::free_image_data(unsigned char* data)
{
	stbi_image_free(data);
}

stb_image::Image stb_image::Image::read_from_memory(const unsigned char* data, int len)
{
	Image result;
	if (result.data_ = stbi_load_from_memory(data, len, &result.width_, &result.height_, &result.channels_, 0))
	{
		result.size_ = result.width_ * result.height_ * result.channels_;
		return result;
	}
	return Image();
}

bool stb_image::Image::read(const std::string_view filename)
{
	set_image_flip(flipFlag_);
	if (data_ = stbi_load(filename.data(), &width_, &height_, &channels_, 0))
	{
		size_ = width_ * height_ * channels_;
		return true;
	}
	return false;
}

void stb_image::Image::set_image_flip(ImageFlip flag)
{
	switch (flag)
	{
	case Image::ImageFlip::NONE:
		stbi_set_flip_vertically_on_load(false);
		break;
	case Image::ImageFlip::VERTICAL:
		stbi_set_flip_vertically_on_load(true);
		break;
	}
}

static void write_callback(void* context, void* data, int size)
{
	std::vector<unsigned char>* out_data = static_cast<std::vector<unsigned char>*>(context);
	size_t current_size = out_data->size();
	out_data->resize(current_size + size);
	std::memcpy(out_data->data() + current_size, data, size);
}

void stb_image::Image::write_to_memory(ImageType type, const unsigned char* data, int width, int height, int channels, std::vector<std::byte>& out_data)
{
	switch (type)
	{
		case ImageType::PNG: {
			int len = 0;
			unsigned char* png_data = stbi_write_png_to_mem(data, width * channels, width, height, channels, &len);
			if (png_data) {
				out_data.resize(len);
				std::memcpy(out_data.data(), png_data, len);
				STBIW_FREE(png_data);
			}
			break;
		}

		case ImageType::JPG: {
			stbi_write_jpg_to_func(write_callback, static_cast<void*>(&out_data), width, height, channels, data, 100);
			break;
		}

		case ImageType::BMP: {
			stbi_write_bmp_to_func(write_callback, static_cast<void*>(&out_data), width, height, channels, data);
			break;
		}

		case ImageType::TGA: {
			stbi_write_tga_to_func(write_callback, static_cast<void*>(&out_data), width, height, channels, data);
			break;
		}
	}
}

stb_image::Image::ImageType stb_image::Image::type(const std::string_view filename)
{
	constexpr std::array<std::pair<const char*, ImageType>, 4> fileTypes = 
	{ 
		std::pair{".png", ImageType::PNG}, 
		{".jpg", ImageType::JPG}, 
		{".bmp", ImageType::BMP}, 
		{".tga", ImageType::TGA}
	};

	for (const auto& [sType, rType] : fileTypes) {
		if (filename.find_last_of(sType) != std::string::npos) {
			return rType;
		}
	}
	return ImageType::NONE;
}

bool stb_image::Image::write(const std::string_view filename)
{
	switch (type(filename))
	{
	case ImageType::PNG:
		return stbi_write_png(filename.data(), width_, height_, channels_, data_, width_ * channels_);
		break;
	case ImageType::JPG:
		return stbi_write_jpg(filename.data(), width_, height_, channels_, data_, 100);
		break;
	case ImageType::BMP:
		return stbi_write_bmp(filename.data(), width_, height_, channels_, data_);
		break;
	case ImageType::TGA:
		return stbi_write_tga(filename.data(), width_, height_, channels_, data_);
		break;
	}

	return false;
};