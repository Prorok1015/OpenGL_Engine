#pragma once
#include <snk/common/snk_api.h>
#include <cstddef>
#include <vector>

namespace img {
	enum class image_type
	{
		none, png, jpg, bmp, tga
	};

	enum class image_flip
	{
		none, vertical
	};

	// Pixels stb decoded from memory; project files reach it only through the resource system, never by a path.
	class SNK_API image
	{
	public:
		image() = default;
		image(const image&) = delete;
		image(image&&) noexcept;
		image& operator=(const image&) = delete;
		image& operator=(image&&) noexcept;
		~image();

		const unsigned char* data() const { return m_data; }
		int channels() const { return m_channels; }
		int width() const { return m_width; }
		int height() const { return m_height; }
		std::size_t size() const { return m_size; }
		// After read_from_memory: the flip stb decoded the rows with (vertical: the first row is the bottom one).
		image_flip flipped() const { return m_flip; }

		// Decodes under the decode_budget of its own header: an unreadable header or a decode over it gives an empty image.
		static image read_from_memory(const unsigned char* data, int len);
		// The size and channel count from the header, without decoding the pixels; false when stb cannot read it.
		static bool info_from_memory(const unsigned char* data, int len, int& width, int& height, int& channels);
		static void set_flip(image_flip flip);
		static void write_to_memory(image_type type, const unsigned char* data, int width, int height, int channels, std::vector<std::byte>& out_data);

	private:
		unsigned char* m_data = nullptr;
		int m_width = 0;
		int m_height = 0;
		int m_channels = 0;
		std::size_t m_size = 0;
		image_flip m_flip = image_flip::none;
	};

	// While alive, what stb holds on this thread may not pass `bytes`: an allocation over it fails, and stb gives the
	// decode up as out of memory (#928). image sets one for every decode by the picture's header. Scopes nest.
	class decode_budget
	{
	public:
		explicit decode_budget(std::size_t bytes);
		~decode_budget();
		decode_budget(const decode_budget&) = delete;
		decode_budget& operator=(const decode_budget&) = delete;

		// Enough for any picture of this header: 12 x its pixel bytes with each side padded by 32, 4 x the file, 256 KiB.
		static std::size_t for_image(int width, int height, int channels, std::size_t encoded_bytes);
		// The most bytes stb held at once on this thread since its last budget began: that of its last image decode.
		static std::size_t peak_bytes();

	private:
		std::size_t m_previous_limit;
		std::size_t m_previous_in_use;
	};
}
