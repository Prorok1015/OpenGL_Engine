#include <snk/render/rnd_capture_commands.h>
#include <snk/command/cmd_base64.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_types.h>
#include <snk/common/logger/engine_log.h>
#include <snk/image/image.h>
#include <snk/render/rnd_image_compare.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_binary.h>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = boost::json;

namespace {
	constexpr std::int64_t max_side = 8192;
	constexpr std::array<std::string_view, 3> reference_extensions{ "png", "jpg", "jpeg" };

	// stb decodes by content (GIF, PSD, HDR... behind a .png name): only the two formats the extension promises pass.
	bool is_png_or_jpeg(std::span<const std::byte> bytes)
	{
		constexpr std::array<std::uint8_t, 8> png{ 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
		constexpr std::array<std::uint8_t, 3> jpeg{ 0xFF, 0xD8, 0xFF };
		const auto starts_with = [bytes](std::span<const std::uint8_t> magic) {
			return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin(),
				[](std::uint8_t expected, std::byte actual) { return std::to_integer<std::uint8_t>(actual) == expected; });
		};
		return starts_with(png) || starts_with(jpeg);
	}

	// The size in the IHDR of a PNG, read here because stb refuses a header over 1 GiB of pixels as unreadable, and
	// such a reference is over the limits, not broken. Nullopt when the first chunk is not an IHDR.
	std::optional<std::pair<std::int64_t, std::int64_t>> png_header_size(std::span<const std::byte> bytes)
	{
		// Signature (8), chunk length (4), chunk type (4), width (4), height (4).
		constexpr std::size_t size_end = 24;
		constexpr std::array<std::uint8_t, 4> ihdr{ 'I', 'H', 'D', 'R' };
		if (bytes.size() < size_end || !std::equal(ihdr.begin(), ihdr.end(), bytes.begin() + 12,
				[](std::uint8_t expected, std::byte actual) { return std::to_integer<std::uint8_t>(actual) == expected; })) {
			return std::nullopt;
		}
		const auto big_endian = [bytes](std::size_t at) {
			std::int64_t value = 0;
			for (std::size_t i = 0; i < 4; ++i) {
				value = (value << 8) | std::to_integer<std::int64_t>(bytes[at + i]);
			}
			return value;
		};
		return std::pair{ big_endian(16), big_endian(20) };
	}

	// Why a reference of `width` x `height` may not be decoded; empty when it may.
	std::string reference_size_problem(std::string_view reference, std::int64_t width, std::int64_t height, std::int64_t pixel_limit)
	{
		if (width <= rnd::capture_compare_max_reference_side && height <= rnd::capture_compare_max_reference_side && width * height <= pixel_limit) {
			return {};
		}
		return std::format("the reference '{}' is {}x{}, over the limits of {} pixels a side and {} pixels",
			reference, width, height, rnd::capture_compare_max_reference_side, pixel_limit);
	}

	// 0 when the argument is absent.
	std::optional<int> side_of(const json::object& args, const char* name)
	{
		const json::value* value = args.if_contains(name);
		if (!value) {
			return 0;
		}
		const auto number = cmd::json_schema::integer_of(*value);
		if (!number || *number < 1 || *number > max_side) {
			return std::nullopt;
		}
		return static_cast<int>(*number);
	}

	// Why `reference` may not be loaded, checked before the resource system sees it; empty when it may.
	std::string reference_problem(std::string_view reference)
	{
		constexpr std::string_view scheme = "res://";
		// Checked before resolve_alias: aliases are registered only by engine code (the model importer, for model tags).
		if (!reference.starts_with(scheme)) {
			return "the reference must be a res:// tag";
		}
		const std::string_view path = reference.substr(scheme.size());
		// res::tag turns a backslash into '/', so "a\..\b" would pass the check below and reach the resolver as "a/../b".
		if (path.find('\\') != std::string_view::npos) {
			return "the reference path must use '/'";
		}
		if (const std::string problem = res::portable_path_problem(path); !problem.empty()) {
			return "the reference path is not allowed: " + problem;
		}
		const std::size_t dot = path.find_last_of('.');
		const std::string_view extension = dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos
			? std::string_view{} : path.substr(dot + 1);
		if (std::ranges::find(reference_extensions, extension) == reference_extensions.end()) {
			return "the reference must be a .png, .jpg or .jpeg picture";
		}
		return {};
	}

	// A number argument in 0..100; `fallback` when it is absent, nullopt when it is not such a number.
	std::optional<double> percent_of(const json::object& args, const char* name, double fallback)
	{
		const json::value* value = args.if_contains(name);
		if (!value) {
			return fallback;
		}
		double number = 0.0;
		if (value->is_double()) {
			number = value->get_double();
		} else if (value->is_int64()) {
			number = static_cast<double>(value->get_int64());
		} else if (value->is_uint64()) {
			number = static_cast<double>(value->get_uint64());
		} else {
			return std::nullopt;
		}
		if (!(number >= 0.0 && number <= 100.0)) {
			return std::nullopt;
		}
		return number;
	}

	cmd::result capture_failure(const rnd::capture_result& captured)
	{
		switch (captured.status) {
		case rnd::capture_status::unknown_view: return cmd::result::failure(cmd::codes::not_found, captured.error);
		case rnd::capture_status::invalid_size: return cmd::result::failure(cmd::codes::invalid_args, captured.error);
		default: return cmd::result::failure(cmd::codes::failed, captured.error);
		}
	}
}

cmd::registration rnd::register_capture_commands(cmd::command_registry& registry, capture_service& service)
{
	cmd::command_desc desc;
	desc.id = "viewport.capture";
	desc.summary = "Capture the final render target of a viewport as a PNG (base64). Size: width and/or height scale it (the side "
		"not given follows the view's proportions; a side over max_side is refused). Without width and height the view is returned "
		"at its size, scaled down to fit max_side (longest side) and max_pixels when they are given, never up";
	desc.category = cmd::agent_category::read;
	desc.needs_display = true;
	desc.args_schema = json::parse(R"json({
		"type": "object",
		"properties": {
			"viewport": { "type": "string", "minLength": 1 },
			"width": { "type": "integer", "minimum": 1, "maximum": 8192,
				"description": "Width of the picture in pixels; without height the height follows the view's proportions" },
			"height": { "type": "integer", "minimum": 1, "maximum": 8192,
				"description": "Height of the picture in pixels; without width the width follows the view's proportions" },
			"max_side": { "type": "integer", "minimum": 1, "maximum": 8192,
				"description": "Largest side of the picture in pixels. With width/height a picture over it is refused; without them the view is scaled down to fit it (proportions kept, never enlarged)" },
			"max_pixels": { "type": "integer", "minimum": 1, "maximum": 33554432,
				"description": "Most pixels in the picture; handled like max_side" },
			"max_source_pixels": { "type": "integer", "minimum": 1, "maximum": 33554432,
				"description": "Refuses a view with more pixels than this before reading it" }
		},
		"additionalProperties": false
	})json");
	desc.result_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"viewport": { "type": "string" },
			"width": { "type": "integer", "minimum": 1 },
			"height": { "type": "integer", "minimum": 1 },
			"format": { "type": "string", "enum": ["png"] },
			"png_base64": { "type": "string", "minLength": 1 }
		},
		"required": ["viewport", "width", "height", "format", "png_base64"],
		"additionalProperties": false
	})");
	desc.invoke = [&service](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		const auto width = side_of(args, "width");
		const auto height = side_of(args, "height");
		if (!width || !height) {
			return cmd::result::failure(cmd::codes::invalid_args, "width and height are integers in 1..8192");
		}
		std::string id;
		if (const json::value* viewport = args.if_contains("viewport")) {
			id = std::string(viewport->as_string());
		}
		if (!id.empty() && !service.has_target(id)) {
			return cmd::result::failure(cmd::codes::not_found, "unknown viewport '" + id + "'");
		}
		if (id.empty()) {
			const std::vector<std::string> ids = service.target_ids();
			if (ids.empty()) {
				return cmd::result::failure(cmd::codes::not_found, "no viewport is registered for capture");
			}
			id = ids.front();
		}

		// Optional ceilings on the picture, for a caller that serves an untrusted client; the derived side counts.
		capture_limits limits;
		limits.fit = *width == 0 && *height == 0 && (args.contains("max_side") || args.contains("max_pixels"));
		if (const json::value* max_side = args.if_contains("max_side")) {
			const auto number = cmd::json_schema::integer_of(*max_side);
			if (!number || *number < 1 || *number > limits.max_side) {
				return cmd::result::failure(cmd::codes::invalid_args, "max_side is an integer in 1..8192");
			}
			limits.max_side = static_cast<int>(*number);
		}
		if (const json::value* max_pixels = args.if_contains("max_pixels")) {
			const auto number = cmd::json_schema::integer_of(*max_pixels);
			if (!number || *number < 1 || *number > limits.max_pixels) {
				return cmd::result::failure(cmd::codes::invalid_args, "max_pixels is an integer in 1..33554432");
			}
			limits.max_pixels = *number;
		}
		if (const json::value* max_source = args.if_contains("max_source_pixels")) {
			const auto number = cmd::json_schema::integer_of(*max_source);
			if (!number || *number < 1 || *number > limits.max_source_pixels) {
				return cmd::result::failure(cmd::codes::invalid_args, "max_source_pixels is an integer in 1..33554432");
			}
			limits.max_source_pixels = *number;
		}

		const capture_result captured = service.capture(id, { *width, *height }, limits);
		if (!captured.ok()) {
			switch (captured.status) {
			case capture_status::unknown_view: return cmd::result::failure(cmd::codes::not_found, captured.error);
			case capture_status::invalid_size: return cmd::result::failure(cmd::codes::invalid_args, captured.error);
			default: return cmd::result::failure(cmd::codes::failed, captured.error);
			}
		}
		const std::vector<std::byte> png = capture_service::encode_png(captured.image);
		if (png.empty()) {
			return cmd::result::failure(cmd::codes::failed, "the capture could not be encoded as PNG");
		}
		return cmd::result::success(json::object{
			{ "viewport", id },
			{ "width", captured.image.size.x },
			{ "height", captured.image.size.y },
			{ "format", "png" },
			{ "png_base64", cmd::base64::encode(png) } });
	};
	return registry.add(std::move(desc));
}

cmd::registration rnd::register_capture_compare_command(cmd::command_registry& registry, capture_service& service, res::resource_system& resources)
{
	cmd::command_desc desc;
	desc.id = "capture.compare";
	desc.summary = "Compare the final render target of a viewport with a res:// PNG/JPEG reference (pixels whose RGB differs by more than fuzz %)";
	desc.category = cmd::agent_category::read;
	desc.needs_display = true;
	desc.args_schema = json::parse(R"json({
		"type": "object",
		"properties": {
			"reference": { "type": "string", "minLength": 1 },
			"viewport": { "type": "string", "minLength": 1 },
			"fuzz": { "type": "number", "minimum": 0, "maximum": 100 },
			"max_diff_pct": { "type": "number", "minimum": 0, "maximum": 100 },
			"max_source_pixels": { "type": "integer", "minimum": 1, "maximum": 33554432 }
		},
		"required": ["reference"],
		"additionalProperties": false
	})json");
	desc.result_schema = json::parse(R"json({
		"type": "object",
		"properties": {
			"match": { "type": "boolean" },
			"diff_pixels": { "type": "integer", "minimum": 0 },
			"total_pixels": { "type": "integer", "minimum": 1 },
			"diff_pct": { "type": "number", "minimum": 0, "maximum": 100 },
			"width": { "type": "integer", "minimum": 1 },
			"height": { "type": "integer", "minimum": 1 },
			"viewport": { "type": "string" },
			"reference": { "type": "string" }
		},
		"required": ["match", "diff_pixels", "total_pixels", "diff_pct", "width", "height", "viewport", "reference"],
		"additionalProperties": false
	})json");
	desc.invoke = [&service, &resources](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		const std::string reference(args.at("reference").as_string());
		if (const std::string problem = reference_problem(reference); !problem.empty()) {
			return cmd::result::failure(cmd::codes::invalid_args, problem);
		}
		const std::optional<double> fuzz = percent_of(args, "fuzz", 4.0);
		const std::optional<double> max_diff_pct = percent_of(args, "max_diff_pct", 2.0);
		if (!fuzz || !max_diff_pct) {
			return cmd::result::failure(cmd::codes::invalid_args, "fuzz and max_diff_pct are numbers in 0..100");
		}
		capture_limits limits;
		if (const json::value* max_source = args.if_contains("max_source_pixels")) {
			const auto number = cmd::json_schema::integer_of(*max_source);
			if (!number || *number < 1 || *number > limits.max_source_pixels) {
				return cmd::result::failure(cmd::codes::invalid_args, "max_source_pixels is an integer in 1..33554432");
			}
			limits.max_source_pixels = *number;
		}
		std::string id = "game";
		if (const json::value* viewport = args.if_contains("viewport")) {
			id = std::string(viewport->as_string());
		}
		if (!service.has_target(id)) {
			return cmd::result::failure(cmd::codes::not_found, "unknown viewport '" + id + "'");
		}

		// The bytes, not a picture_resource: the picture adapter would decode the whole file before any limit is checked.
		const res::res_handle<res::binary_resource> handle = resources.require_sync<res::binary_resource>(res::tag{ reference });
		handle.wait();
		const std::shared_ptr<res::binary_resource> file = handle.get();
		if (!file) {
			// The resolver's error may name a path on disk: logged, not returned.
			const auto block = handle.get_control_block();
			egLOG_WARN("render", "capture.compare: the reference '{}' cannot be loaded: {}", reference,
				block && !block->error_msg.empty() ? block->error_msg : std::string("no data"));
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' cannot be loaded", reference));
		}
		const std::vector<std::byte>& bytes = file->bytes();
		if (bytes.size() > capture_compare_max_reference_bytes) {
			return cmd::result::failure(cmd::codes::invalid_args, std::format("the reference '{}' is {} bytes, over the limit of {} bytes",
				reference, bytes.size(), capture_compare_max_reference_bytes));
		}
		if (!is_png_or_jpeg(bytes)) {
			return cmd::result::failure(cmd::codes::invalid_args, std::format("the reference '{}' is not a PNG or JPEG file", reference));
		}
		const std::int64_t pixel_limit = std::min(capture_service::max_pixels, limits.max_source_pixels);
		if (const auto png_size = png_header_size(bytes)) {
			if (std::string problem = reference_size_problem(reference, png_size->first, png_size->second, pixel_limit); !problem.empty()) {
				return cmd::result::failure(cmd::codes::invalid_args, std::move(problem));
			}
		}
		const auto* encoded = reinterpret_cast<const unsigned char*>(bytes.data());
		const int encoded_length = static_cast<int>(bytes.size());
		int width = 0;
		int height = 0;
		int channels = 0;
		if (!stb_image::Image::info_from_memory(encoded, encoded_length, width, height, channels) || width <= 0 || height <= 0) {
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' has no readable picture header", reference));
		}
		if (std::string problem = reference_size_problem(reference, width, height, pixel_limit); !problem.empty()) {
			return cmd::result::failure(cmd::codes::invalid_args, std::move(problem));
		}

		const capture_result captured = service.capture(id, { 0, 0 }, limits);
		if (!captured.ok()) {
			return capture_failure(captured);
		}
		const glm::ivec2 size = captured.image.size;
		if (size.x != width || size.y != height) {
			return cmd::result::failure(capture_size_mismatch, std::format("size_mismatch: the capture of viewport '{}' is {}x{}, the reference '{}' is {}x{}",
				id, size.x, size.y, reference, width, height));
		}

		const stb_image::Image picture = stb_image::Image::read_from_memory(encoded, encoded_length);
		if (picture.data() == nullptr || picture.width() != width || picture.height() != height) {
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' cannot be decoded", reference));
		}
		// stb's vertical flip is global (pct_adapter turns it on); the decode records the one it ran with.
		const bool bottom_up = picture.flipped() == stb_image::Image::ImageFlip::VERTICAL;
		const std::vector<std::byte> expected = to_rgba8_top_down(
			std::span<const std::byte>(reinterpret_cast<const std::byte*>(picture.data()), picture.size()),
			width, height, picture.channels_count(), bottom_up);
		if (expected.empty()) {
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' has {} channels", reference, picture.channels_count()));
		}
		const std::optional<image_diff> diff = compare_rgb_of_rgba8({ size.x, size.y, captured.image.rgba }, { size.x, size.y, expected }, *fuzz);
		if (!diff) {
			return cmd::result::failure(cmd::codes::failed, "the capture and the reference cannot be compared");
		}
		return cmd::result::success(json::object{
			{ "match", diff->diff_pct <= *max_diff_pct },
			{ "diff_pixels", diff->diff_pixels },
			{ "total_pixels", diff->total_pixels },
			{ "diff_pct", diff->diff_pct },
			{ "width", size.x },
			{ "height", size.y },
			{ "viewport", id },
			{ "reference", reference } });
	};
	return registry.add(std::move(desc));
}
