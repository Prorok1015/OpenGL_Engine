#include <snk/render/rnd_capture_service.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <snk/common/logger/engine_log.h>
#include <snk/image/image.h>

namespace {
	constexpr int pixel_bytes = 4;

	// The driver gives the bottom row first (GL), a picture starts at the top.
	std::vector<std::byte> flip_rows(const std::vector<std::byte>& rows, int width, int height)
	{
		const std::size_t stride = static_cast<std::size_t>(width) * pixel_bytes;
		std::vector<std::byte> out(rows.size());
		for (int y = 0; y < height; ++y) {
			std::memcpy(out.data() + static_cast<std::size_t>(y) * stride, rows.data() + static_cast<std::size_t>(height - 1 - y) * stride, stride);
		}
		return out;
	}

	std::vector<std::byte> scale_nearest(const std::vector<std::byte>& pixels, glm::ivec2 from, glm::ivec2 to)
	{
		std::vector<std::byte> out(static_cast<std::size_t>(to.x) * static_cast<std::size_t>(to.y) * pixel_bytes);
		for (int y = 0; y < to.y; ++y) {
			const int source_y = static_cast<int>(static_cast<std::int64_t>(y) * from.y / to.y);
			for (int x = 0; x < to.x; ++x) {
				const int source_x = static_cast<int>(static_cast<std::int64_t>(x) * from.x / to.x);
				std::memcpy(out.data() + (static_cast<std::size_t>(y) * to.x + x) * pixel_bytes,
					pixels.data() + (static_cast<std::size_t>(source_y) * from.x + source_x) * pixel_bytes, pixel_bytes);
			}
		}
		return out;
	}

	std::int64_t pixels_of(glm::ivec2 size)
	{
		return static_cast<std::int64_t>(size.x) * size.y;
	}

	// The size a request asks for; a component that is 0 follows the aspect of the source. Nullopt when a side
	// (the derived one included) or the pixel count is over the limits.
	std::optional<glm::ivec2> requested_size(glm::ivec2 source, glm::ivec2 wanted, const rnd::capture_limits& limits)
	{
		if (wanted.x <= 0 && wanted.y <= 0) {
			// The target itself. Under the service defaults it was checked against the pixel budget already; a caller
			// with tighter limits gets a refusal instead of a picture bigger than it asked for.
			const bool tighter = limits.max_side < rnd::capture_service::max_side || limits.max_pixels < rnd::capture_service::max_pixels;
			if (tighter && (source.x > limits.max_side || source.y > limits.max_side || pixels_of(source) > limits.max_pixels)) {
				return std::nullopt;
			}
			return source;
		}
		// 64-bit: a request of 8192 on a 1 x 8192 target derives 8192 * 8192.
		if (wanted.x <= 0) {
			const std::int64_t derived = std::max<std::int64_t>(1, static_cast<std::int64_t>(wanted.y) * source.x / source.y);
			if (derived > limits.max_side) {
				return std::nullopt;
			}
			wanted.x = static_cast<int>(derived);
		}
		if (wanted.y <= 0) {
			const std::int64_t derived = std::max<std::int64_t>(1, static_cast<std::int64_t>(wanted.x) * source.y / source.x);
			if (derived > limits.max_side) {
				return std::nullopt;
			}
			wanted.y = static_cast<int>(derived);
		}
		if (wanted.x > limits.max_side || wanted.y > limits.max_side || pixels_of(wanted) > limits.max_pixels) {
			return std::nullopt;
		}
		return wanted;
	}
}

rnd::capture_service::capture_service(driver::driver_interface& drv)
	: m_driver(drv)
{
}

cmd::registration rnd::capture_service::add_target(std::string id, target_provider provider)
{
	if (id.empty() || !provider) {
		egLOG_ERROR("render", "capture target needs an id and a provider");
		return {};
	}
	if (has_target(id)) {
		egLOG_ERROR("render", "capture target '{}' is already registered", id);
		return {};
	}
	const std::uint64_t key = m_next_key++;
	m_targets.push_back({ key, std::move(id), std::move(provider) });
	return cmd::registration(m_alive, [this, key] {
		std::erase_if(m_targets, [key](const target& entry) { return entry.key == key; });
	});
}

bool rnd::capture_service::has_target(std::string_view id) const
{
	return std::ranges::any_of(m_targets, [id](const target& entry) { return entry.id == id; });
}

std::vector<std::string> rnd::capture_service::target_ids() const
{
	std::vector<std::string> ids;
	ids.reserve(m_targets.size());
	for (const target& entry : m_targets) {
		ids.push_back(entry.id);
	}
	return ids;
}

rnd::capture_result rnd::capture_service::capture(std::string_view id, glm::ivec2 size, capture_limits limits) const
{
	// The service limits are the ceiling: a caller can ask for less, never for more.
	limits.max_side = std::min(limits.max_side, max_side);
	limits.max_pixels = std::min(limits.max_pixels, max_pixels);
	capture_result result;
	const target* found = nullptr;
	if (id.empty()) {
		found = m_targets.empty() ? nullptr : &m_targets.front();
	} else {
		const auto it = std::ranges::find_if(m_targets, [id](const target& entry) { return entry.id == id; });
		found = it == m_targets.end() ? nullptr : &*it;
	}
	if (!found) {
		result.status = capture_status::unknown_view;
		result.error = id.empty() ? "no viewport is registered for capture" : std::format("unknown viewport '{}'", id);
		return result;
	}

	driver::texture_interface* texture = found->provider();
	result.status = capture_status::failed;
	if (!texture) {
		result.error = std::format("viewport '{}' has no render target yet", found->id);
		return result;
	}
	const glm::ivec2 source{ static_cast<int>(texture->width()), static_cast<int>(texture->height()) };
	// Before anything is allocated: the readback of the target is the biggest buffer a capture makes.
	if (pixels_of(source) > max_pixels) {
		result.error = std::format("the render target of viewport '{}' ({}x{}) is too big to capture", found->id, source.x, source.y);
		return result;
	}
	const std::optional<glm::ivec2> target_size = requested_size(source, size, limits);
	if (!target_size) {
		result.status = capture_status::invalid_size;
		result.error = std::format("size {}x{} for a {}x{} target is over the capture limits ({} px a side, {} pixels)",
			size.x, size.y, source.x, source.y, limits.max_side, limits.max_pixels);
		return result;
	}
	std::vector<std::byte> rows = m_driver.read_texture(texture->handle(), { 0, 0, source.x, source.y });
	if (rows.size() != static_cast<std::size_t>(source.x) * static_cast<std::size_t>(source.y) * pixel_bytes || rows.empty()) {
		result.error = std::format("the render target of viewport '{}' ({}x{}) cannot be read back", found->id, source.x, source.y);
		return result;
	}

	result.status = capture_status::ok;
	result.image.rgba = flip_rows(rows, source.x, source.y);
	result.image.size = source;
	if (*target_size != source) {
		result.image.rgba = scale_nearest(result.image.rgba, source, *target_size);
		result.image.size = *target_size;
	}
	return result;
}

std::vector<std::byte> rnd::capture_service::encode_png(const captured_image& image)
{
	std::vector<std::byte> png;
	const std::size_t count = static_cast<std::size_t>(std::max(image.size.x, 0)) * static_cast<std::size_t>(std::max(image.size.y, 0));
	if (count == 0 || image.rgba.size() != count * pixel_bytes) {
		return png;
	}
	std::vector<unsigned char> rgb(count * 3);
	for (std::size_t i = 0; i < count; ++i) {
		for (std::size_t c = 0; c < 3; ++c) {
			rgb[i * 3 + c] = static_cast<unsigned char>(image.rgba[i * pixel_bytes + c]);
		}
	}
	stb_image::Image::write_to_memory(stb_image::Image::ImageType::PNG, rgb.data(), image.size.x, image.size.y, 3, png);
	return png;
}
