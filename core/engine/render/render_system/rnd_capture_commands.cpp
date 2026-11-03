#include <snk/render/rnd_capture_commands.h>
#include <snk/command/cmd_base64.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_types.h>
#include <snk/render/rnd_image_compare.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/resource/res_path_sanitize.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/resource/resources/res_resource_picture.h>

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
		// Only pictures stb decodes: their orientation is known (pct_adapter flips them bottom-up).
		const std::size_t dot = path.find_last_of('.');
		const std::string_view extension = dot == std::string_view::npos || path.find('/', dot) != std::string_view::npos
			? std::string_view{} : path.substr(dot + 1);
		if (std::ranges::find(res::pct_adapter::EXTENSIONS, extension) == res::pct_adapter::EXTENSIONS.end()) {
			return "the reference must be a .png or .jpg picture";
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
	desc.summary = "Capture the final render target of a viewport as a PNG (base64), optionally scaled";
	desc.category = cmd::agent_category::read;
	desc.needs_display = true;
	desc.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"viewport": { "type": "string", "minLength": 1 },
			"width": { "type": "integer", "minimum": 1, "maximum": 8192 },
			"height": { "type": "integer", "minimum": 1, "maximum": 8192 },
			"max_side": { "type": "integer", "minimum": 1, "maximum": 8192 },
			"max_pixels": { "type": "integer", "minimum": 1, "maximum": 33554432 },
			"max_source_pixels": { "type": "integer", "minimum": 1, "maximum": 33554432 }
		},
		"additionalProperties": false
	})");
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
	desc.summary = "Compare the final render target of a viewport with a res:// reference picture (pixels whose channels differ by more than fuzz %)";
	desc.category = cmd::agent_category::read;
	desc.needs_display = true;
	desc.args_schema = json::parse(R"json({
		"type": "object",
		"properties": {
			"reference": { "type": "string", "minLength": 1 },
			"viewport": { "type": "string", "minLength": 1 },
			"fuzz": { "type": "number", "minimum": 0, "maximum": 100 },
			"max_diff_pct": { "type": "number", "minimum": 0, "maximum": 100 }
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
		std::string id = "game";
		if (const json::value* viewport = args.if_contains("viewport")) {
			id = std::string(viewport->as_string());
		}
		if (!service.has_target(id)) {
			return cmd::result::failure(cmd::codes::not_found, "unknown viewport '" + id + "'");
		}

		const res::res_handle<res::picture_resource> handle = resources.require_sync<res::picture_resource>(res::tag{ reference });
		handle.wait();
		const std::shared_ptr<res::picture_resource> picture = handle.get();
		if (!picture) {
			const auto block = handle.get_control_block();
			const std::string why = block && !block->error_msg.empty() ? block->error_msg : std::string("no picture");
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' cannot be loaded: {}", reference, why));
		}
		const glm::ivec2 reference_size = picture->size();
		if (reference_size.x <= 0 || reference_size.y <= 0 || picture->data() == nullptr) {
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' is an empty picture", reference));
		}
		const std::int64_t reference_pixels = static_cast<std::int64_t>(reference_size.x) * reference_size.y;
		if (reference_pixels > capture_service::max_pixels) {
			return cmd::result::failure(cmd::codes::invalid_args, std::format("the reference '{}' ({}x{}) has more than {} pixels",
				reference, reference_size.x, reference_size.y, capture_service::max_pixels));
		}
		const std::size_t reference_bytes = static_cast<std::size_t>(reference_pixels) * static_cast<std::size_t>(std::max(picture->channels(), 0));
		// pct_adapter decodes with stb's vertical flip on: the first row of the picture is its bottom one.
		const std::vector<std::byte> expected = to_rgba8_top_down(
			std::span<const std::byte>(reinterpret_cast<const std::byte*>(picture->data()), reference_bytes),
			reference_size.x, reference_size.y, picture->channels(), true);
		if (expected.empty()) {
			return cmd::result::failure(cmd::codes::failed, std::format("the reference '{}' has {} channels", reference, picture->channels()));
		}

		capture_result captured = service.capture(id);
		if (!captured.ok()) {
			return capture_failure(captured);
		}
		const glm::ivec2 size = captured.image.size;
		if (size != reference_size) {
			return cmd::result::failure(capture_size_mismatch, std::format("size_mismatch: the capture of viewport '{}' is {}x{}, the reference '{}' is {}x{}",
				id, size.x, size.y, reference, reference_size.x, reference_size.y));
		}
		// viewport.capture saves the frame without alpha, so a reference made from it is opaque: so is the capture here.
		for (std::size_t alpha = 3; alpha < captured.image.rgba.size(); alpha += 4) {
			captured.image.rgba[alpha] = std::byte{ 255 };
		}
		const std::optional<image_diff> diff = compare_rgba8({ size.x, size.y, captured.image.rgba }, { size.x, size.y, expected }, *fuzz);
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
