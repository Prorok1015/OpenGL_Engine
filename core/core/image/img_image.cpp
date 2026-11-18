#include <snk/image/img_image.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
	constexpr std::size_t no_budget = std::numeric_limits<std::size_t>::max();
	// In front of every stb block, its size: free and realloc give the bytes back. As wide as malloc aligns.
	constexpr std::size_t block_header = std::max(alignof(std::max_align_t), 2 * sizeof(void*));

	thread_local std::size_t t_budget = no_budget;
	thread_local std::size_t t_in_use = 0;
	thread_local std::size_t t_peak = 0;

	void* budget_malloc(std::size_t size)
	{
		if (size > t_budget || t_in_use > t_budget - size || size > no_budget - block_header) {
			return nullptr;
		}
		auto* block = static_cast<unsigned char*>(std::malloc(block_header + size));
		if (!block) {
			return nullptr;
		}
		std::memcpy(block, &size, sizeof(size));
		// Counted under a budget only: what a thread decodes without one is often freed on another thread.
		t_in_use = t_budget == no_budget ? 0 : t_in_use + size;
		t_peak = std::max(t_peak, t_in_use);
		return block + block_header;
	}

	void budget_free(void* memory)
	{
		if (!memory) {
			return;
		}
		auto* block = static_cast<unsigned char*>(memory) - block_header;
		std::size_t size = 0;
		std::memcpy(&size, block, sizeof(size));
		// A picture is freed on any thread, not only the one that decoded it: never below zero.
		t_in_use -= std::min(t_in_use, size);
		std::free(block);
	}

	// Both blocks count until the copy is done, as with a realloc that moves.
	void* budget_realloc_sized(void* memory, std::size_t old_size, std::size_t new_size)
	{
		void* grown = budget_malloc(new_size);
		if (!grown) {
			return nullptr;
		}
		if (memory) {
			std::memcpy(grown, memory, std::min(old_size, new_size));
			budget_free(memory);
		}
		return grown;
	}
}

#define STBI_MALLOC(size) budget_malloc(size)
#define STBI_REALLOC_SIZED(memory, old_size, new_size) budget_realloc_sized(memory, old_size, new_size)
#define STBI_FREE(memory) budget_free(memory)

#include <stb_image.h>
#include <stb_image_write.h>

#include <mutex>
#include <shared_mutex>
#include <utility>

namespace {
	// stb has no getter for its global flip: kept here so a decode can tell which way its rows came out. Decodes
	// hold the lock shared, a flip change exclusively, so the recorded flip is the one stb used.
	std::shared_mutex g_image_flip_mutex;
	img::image_flip g_image_flip = img::image_flip::none;

	void append_to_vector(void* context, void* data, int size)
	{
		auto* out_data = static_cast<std::vector<std::byte>*>(context);
		const std::size_t current_size = out_data->size();
		out_data->resize(current_size + static_cast<std::size_t>(size));
		std::memcpy(out_data->data() + current_size, data, static_cast<std::size_t>(size));
	}
}

img::image::image(image&& rhs) noexcept
{
	*this = std::move(rhs);
}

img::image& img::image::operator=(image&& rhs) noexcept
{
	std::swap(m_data, rhs.m_data);
	m_width = rhs.m_width;
	m_height = rhs.m_height;
	m_channels = rhs.m_channels;
	m_size = rhs.m_size;
	m_flip = rhs.m_flip;
	return *this;
}

img::image::~image()
{
	stbi_image_free(m_data);
}

img::image img::image::read_from_memory(const unsigned char* data, int len)
{
	// The header is only a claim: a tiny PNG can still inflate to gigabytes, so the decoder gets what that header needs.
	int width = 0;
	int height = 0;
	int channels = 0;
	if (len <= 0 || !stbi_info_from_memory(data, len, &width, &height, &channels)) {
		return image();
	}
	const decode_budget budget(decode_budget::for_image(width, height, channels, static_cast<std::size_t>(len)));
	image result;
	std::shared_lock lock(g_image_flip_mutex);
	result.m_flip = g_image_flip;
	if ((result.m_data = stbi_load_from_memory(data, len, &result.m_width, &result.m_height, &result.m_channels, 0)))
	{
		result.m_size = static_cast<std::size_t>(result.m_width) * static_cast<std::size_t>(result.m_height) * static_cast<std::size_t>(result.m_channels);
		return result;
	}
	return image();
}

bool img::image::info_from_memory(const unsigned char* data, int len, int& width, int& height, int& channels)
{
	return stbi_info_from_memory(data, len, &width, &height, &channels) != 0;
}

void img::image::set_flip(image_flip flip)
{
	std::unique_lock lock(g_image_flip_mutex);
	g_image_flip = flip;
	stbi_set_flip_vertically_on_load(flip == image_flip::vertical);
}

void img::image::write_to_memory(image_type type, const unsigned char* data, int width, int height, int channels, std::vector<std::byte>& out_data)
{
	switch (type)
	{
		case image_type::png: {
			int len = 0;
			unsigned char* png_data = stbi_write_png_to_mem(data, width * channels, width, height, channels, &len);
			if (png_data) {
				out_data.resize(static_cast<std::size_t>(len));
				std::memcpy(out_data.data(), png_data, static_cast<std::size_t>(len));
				STBIW_FREE(png_data);
			}
			break;
		}

		case image_type::jpg:
			stbi_write_jpg_to_func(append_to_vector, &out_data, width, height, channels, data, 100);
			break;

		case image_type::bmp:
			stbi_write_bmp_to_func(append_to_vector, &out_data, width, height, channels, data);
			break;

		case image_type::tga:
			stbi_write_tga_to_func(append_to_vector, &out_data, width, height, channels, data);
			break;

		case image_type::none:
			break;
	}
}

img::decode_budget::decode_budget(std::size_t bytes)
	: m_previous_limit(t_budget)
	, m_previous_in_use(t_in_use)
{
	t_budget = bytes;
	t_in_use = 0;
	t_peak = 0;
}

img::decode_budget::~decode_budget()
{
	t_budget = m_previous_limit;
	t_in_use = m_previous_in_use;
}

std::size_t img::decode_budget::for_image(int width, int height, int channels, std::size_t encoded_bytes)
{
	if (width <= 0 || height <= 0 || channels <= 0) {
		return 0;
	}
	// The most stb holds at once is 10 x the header's pixel bytes: a 16-bit grey interlaced PNG with tRNS (the inflate
	// buffer after it doubled 4, the de-interlaced image 4, one pass 2); JPEG and HDR stay under 5. A JPEG rounds each
	// side up to its MCU (at most 31 more); a PNG keeps its IDAT in a buffer that doubles (3 x the file while it
	// grows); the decoder structs are tens of kilobytes.
	constexpr std::uint64_t margin = 12;
	constexpr std::uint64_t side_padding = 32;
	constexpr std::uint64_t fixed_bytes = 256 * 1024;
	// Far over what a caller lets through; below it the sum cannot overflow.
	constexpr std::uint64_t counted_up_to = std::uint64_t{ 1 } << 40;
	const std::uint64_t pixels = (static_cast<std::uint64_t>(width) + side_padding) * (static_cast<std::uint64_t>(height) + side_padding);
	const std::uint64_t encoded = static_cast<std::uint64_t>(encoded_bytes);
	if (pixels > counted_up_to / static_cast<std::uint64_t>(channels) || encoded > counted_up_to) {
		return no_budget;
	}
	const std::uint64_t total = margin * pixels * static_cast<std::uint64_t>(channels) + 4 * encoded + fixed_bytes;
	return static_cast<std::size_t>(std::min<std::uint64_t>(total, no_budget));
}

std::size_t img::decode_budget::peak_bytes()
{
	return t_peak;
}
