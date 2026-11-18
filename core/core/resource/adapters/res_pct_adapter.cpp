#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/image/img_image.h>
#include <snk/common/logger/engine_log.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
	void copy_rows_reversed(const unsigned char* source, unsigned char* target, std::size_t row_bytes, std::size_t rows)
	{
		for (std::size_t row = 0; row < rows; ++row) {
			std::memcpy(target + (rows - 1 - row) * row_bytes, source + row * row_bytes, row_bytes);
		}
	}
}

std::optional<res::raw_image_adapter::raw_image_header> res::raw_image_adapter::read_header(const std::vector<std::byte>& data) const
{
	// The bytes are a file's: one too short has no header, which is the caller's to report, never an assert (#930).
	if (data.size() < HEADER_SIZE) {
		return {};
	}

	auto* header_ptr = reinterpret_cast<const raw_image_header*>(data.data());
	raw_image_header header = *header_ptr;
	return header;
}

std::shared_ptr<res::picture_resource> res::raw_image_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	// A null result would be stored as a ready resource with no data: failures throw, and the resource system settles
	// the block with the error.
	if (data.size() < HEADER_SIZE) {
		throw std::runtime_error("raw image '" + std::string{ tag.view() } + "' is shorter than its header");
	}
	auto op_header = read_header(data);
	if (!op_header) {
		throw std::runtime_error("raw image '" + std::string{ tag.view() } + "' has no readable header");
	}
	auto& header = *op_header;
	const std::size_t available = data.size() - HEADER_SIZE;
	// Divided, not multiplied out: a corrupt header must not overflow the size it is checked against.
	if (header.size.x <= 0 || header.size.y <= 0 || header.channels <= 0 ||
		static_cast<std::size_t>(header.size.x) * static_cast<std::size_t>(header.channels) > available / static_cast<std::size_t>(header.size.y)) {
		throw std::runtime_error("raw image '" + std::string{ tag.view() } + "' has a header that does not match its data");
	}
	const std::size_t byte_count = static_cast<std::size_t>(header.size.x) * static_cast<std::size_t>(header.size.y) * static_cast<std::size_t>(header.channels);
	auto raw_data = new unsigned char[byte_count];
	auto* data_ptr = reinterpret_cast<const unsigned char*>(data.data() + HEADER_SIZE);
	auto* data_end = data_ptr + byte_count;
	std::copy(data_ptr, data_end, raw_data);

	return std::make_shared<res::picture_resource>(tag, header.size, header.channels, raw_data);
}

std::vector<std::byte> res::raw_image_adapter::serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const
{
	auto pic_res = std::static_pointer_cast<res::picture_resource>(resource);
	if (!pic_res) {
		ASSERT_FAIL("Resource is not picture resource!");
		return {};
	}

	raw_image_header header;
	header.size = pic_res->size();
	header.channels = pic_res->channels();

	std::vector<std::byte> data;
	data.resize(HEADER_SIZE + pic_res->size().x * pic_res->size().y * pic_res->channels());
	auto* header_ptr = reinterpret_cast<raw_image_header*>(data.data());
	*header_ptr = header;
	auto* data_ptr = reinterpret_cast<unsigned char*>(data.data() + HEADER_SIZE);
	std::memcpy(data_ptr, pic_res->data(), pic_res->size().x * pic_res->size().y * pic_res->channels());
	return data;
}

res::pct_adapter::pct_adapter()
{
	img::image::set_flip(img::image_flip::vertical);
}

std::size_t res::pct_adapter::decode_budget_bytes(int width, int height, int channels, std::size_t encoded_bytes)
{
	return img::decode_budget::for_image(width, height, channels, encoded_bytes);
}

std::size_t res::pct_adapter::last_decode_peak_bytes()
{
	return img::decode_budget::peak_bytes();
}

std::shared_ptr<res::picture_resource> res::pct_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw std::runtime_error("picture '" + std::string{ tag.view() } + "' is too large to decode: " + std::to_string(data.size()) + " bytes");
	}
	// The header first: a few megabytes of file can say they are gigabytes of pixels, and several pictures decode at
	// once on the workers. Nothing is allocated for one over the budget (#928).
	int width = 0;
	int height = 0;
	int channels = 0;
	if (!img::image::info_from_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()), width, height, channels)) {
		throw std::runtime_error("picture '" + std::string{ tag.view() } + "' is not a decodable image");
	}
	const bool fits = width > 0 && height > 0 && channels > 0 && width <= max_side && height <= max_side
		&& static_cast<std::size_t>(width) * static_cast<std::size_t>(height) <= max_decoded_bytes / static_cast<std::size_t>(channels);
	if (!fits) {
		throw std::runtime_error("picture '" + std::string{ tag.view() } + "' is " + std::to_string(width) + "x" + std::to_string(height)
			+ " with " + std::to_string(channels) + " channels: too large to decode (at most " + std::to_string(max_side)
			+ " a side and " + std::to_string(max_decoded_bytes / (1024 * 1024)) + " MiB of pixels)");
	}
	const img::image picture = img::image::read_from_memory(reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
	if (!picture.data()) {
		throw std::runtime_error("picture '" + std::string{ tag.view() } + "' is not a decodable image");
	}
	glm::ivec2 size(picture.width(), picture.height());
	auto raw_data = new unsigned char[picture.size()];
	if (picture.flipped() == img::image_flip::vertical) {
		std::memcpy(raw_data, picture.data(), picture.size());
	} else {
		const std::size_t row_bytes = static_cast<std::size_t>(picture.width()) * static_cast<std::size_t>(picture.channels());
		copy_rows_reversed(picture.data(), raw_data, row_bytes, static_cast<std::size_t>(picture.height()));
	}
	return std::make_shared<res::picture_resource>(tag, size, picture.channels(), raw_data);
}

std::vector<std::byte> res::pct_adapter::serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const
{
	auto pic_res = std::static_pointer_cast<res::picture_resource>(resource);
	if (!pic_res) {
		ASSERT_FAIL("Resource is not picture resource!");
		return {};
	}
	// A file holds its top row first.
	const std::size_t row_bytes = static_cast<std::size_t>(pic_res->size().x) * static_cast<std::size_t>(pic_res->channels());
	const std::size_t rows = static_cast<std::size_t>(pic_res->size().y);
	std::vector<unsigned char> top_down(row_bytes * rows);
	copy_rows_reversed(pic_res->data(), top_down.data(), row_bytes, rows);
	std::vector<std::byte> out_data;
	img::image::write_to_memory(img::image_type::jpg, top_down.data(), pic_res->size().x, pic_res->size().y, pic_res->channels(), out_data);
	return out_data;
}