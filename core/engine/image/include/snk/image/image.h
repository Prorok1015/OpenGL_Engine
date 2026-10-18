#pragma once
#include <string_view>
#include <vector>

#ifndef IMAGE
#define IMAGE
namespace stb_image {
	class Image
	{
	public:
		enum class ImageType
		{
			NONE, PNG, JPG, BMP, TGA
		};

		enum class ImageFlip
		{
			NONE, VERTICAL
		};
	public:
		Image() = default;
		Image(const std::string_view filename);
		Image(const Image&) = delete;
		Image(Image&&) noexcept;
		Image& operator= (const Image&) = delete;
		Image& operator= (Image&&) noexcept;
		~Image();

		bool open(const std::string_view filename) { return read(filename); }
		ImageType type(const std::string_view filename);
		bool write(const std::string_view filename);
		unsigned char* pop_data() { auto res = data_; data_ = nullptr; return res; };
		const unsigned char* data() const { return data_; }
		int channels_count() const { return channels_; };
		int width() const { return width_; };
		int height() const { return height_; };
		int size() const { return size_; }
		void flip(ImageFlip fp) { flipFlag_ = fp; }

		static void free_image_data(unsigned char* data);
		static Image read_from_memory(const unsigned char* data, int len);
		static void set_image_flip(ImageFlip flag);
		static void write_to_memory(ImageType type, const unsigned char* data, int width, int height, int channels, std::vector<std::byte>& out_data);
	
	private:
		bool read(const std::string_view filename);

	private:
		unsigned char* data_ = nullptr;
		int width_ = 0;
		int height_ = 0;
		int channels_ = 0;
		size_t size_ = 0;
		ImageFlip flipFlag_ = ImageFlip::NONE;
	};
}
#endif // IMAGE