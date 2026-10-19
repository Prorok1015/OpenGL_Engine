#include <snk/render/rnd_capture_commands.h>
#include <snk/command/cmd_base64.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_types.h>

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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
}

cmd::registration rnd::register_capture_commands(cmd::command_registry& registry, capture_service& service)
{
	cmd::command_desc desc;
	desc.id = "viewport.capture";
	desc.summary = "Capture the final render target of a viewport as a PNG (base64), optionally scaled";
	desc.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"viewport": { "type": "string", "minLength": 1 },
			"width": { "type": "integer", "minimum": 1, "maximum": 8192 },
			"height": { "type": "integer", "minimum": 1, "maximum": 8192 }
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

		const capture_result captured = service.capture(id, { *width, *height });
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
