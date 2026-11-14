#pragma once
#include <cstddef>
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
		size_t size() const { return size_; }
		void flip(ImageFlip fp) { flipFlag_ = fp; }
		// After read_from_memory: the flip stb decoded the rows with (VERTICAL: the first row is the bottom one).
		ImageFlip flipped() const { return flipFlag_; }

		static void free_image_data(unsigned char* data);
		static Image read_from_memory(const unsigned char* data, int len);
		// The size and channel count from the header, without decoding the pixels; false when stb cannot read it.
		static bool info_from_memory(const unsigned char* data, int len, int& width, int& height, int& channels);
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

	// While alive, what stb holds on this thread may not pass `bytes`: an allocation over it fails, and stb gives the
	// decode up as out of memory (#928). Without one a decode is unlimited. Scopes nest.
	class decode_budget
	{
	public:
		explicit decode_budget(std::size_t bytes);
		~decode_budget();
		decode_budget(const decode_budget&) = delete;
		decode_budget& operator=(const decode_budget&) = delete;

		// Enough for any picture of this header: 12 x its pixel bytes with each side padded by 32, 4 x the file, 256 KiB.
		static std::size_t for_image(int width, int height, int channels, std::size_t encoded_bytes);
		// The most bytes stb held at once on this thread since its last budget began.
		static std::size_t peak_bytes();

	private:
		std::size_t m_previous_limit;
		std::size_t m_previous_in_use;
	};
}
#endif // IMAGE