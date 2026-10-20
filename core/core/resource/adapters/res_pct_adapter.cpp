#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/image/image.h>
#include <snk/common/logger/engine_log.h>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

std::optional<res::raw_image_adapter::raw_image_header> res::raw_image_adapter::read_header(const std::vector<std::byte>& data) const
{
	if (data.size() < HEADER_SIZE) {
		ASSERT_FAIL("Data size is less than header size!");
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
	stb_image::Image::set_image_flip(stb_image::Image::ImageFlip::VERTICAL);
}

std::shared_ptr<res::picture_resource> res::pct_adapter::operator()(res::tag tag, const std::vector<std::byte>& data) const
{
	if (data.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw std::runtime_error("picture '" + std::string{ tag.view() } + "' is too large to decode: " + std::to_string(data.size()) + " bytes");
	}
	auto img = stb_image::Image::read_from_memory( reinterpret_cast<const unsigned char*>(data.data()), static_cast<int>(data.size()));
	if (!img.data()) {
		throw std::runtime_error("picture '" + std::string{ tag.view() } + "' is not a decodable image");
	}
	glm::ivec2 size(img.width(), img.height());
	auto raw_data = new unsigned char[img.size()];
	std::memcpy(raw_data, img.data(), img.size());
	return std::make_shared<res::picture_resource>(tag, size, img.channels_count(), raw_data);
}

std::vector<std::byte> res::pct_adapter::serialize(const res::tag& tag, const std::shared_ptr<res::resource_entry>& resource) const
{
	auto pic_res = std::static_pointer_cast<res::picture_resource>(resource);
	if (!pic_res) {
		ASSERT_FAIL("Resource is not picture resource!");
		return {};
	}
	std::vector<std::byte> out_data;
	stb_image::Image::write_to_memory(stb_image::Image::ImageType::JPG, pic_res->data(), pic_res->size().x, pic_res->size().y, pic_res->channels(), out_data);
	return out_data;
}