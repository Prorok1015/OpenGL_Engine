#include <snk/application/app_frame_commands.h>
#include <snk/command/cmd_json_schema.h>
#include <snk/command/cmd_types.h>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <cstdint>
#include <optional>
#include <utility>

namespace json = boost::json;

namespace {
	// Keeps one call from queueing a run of days.
	constexpr std::int64_t max_frames = 1'000'000;
}

app::frame_command_registrations app::register_frame_commands(cmd::command_registry& registry, frame_control& control)
{
	cmd::command_desc desc;
	desc.id = "app.advance";
	desc.summary = "Run the next N frames of simulation time and hold the time between calls (deterministic stepping); app.resume releases the hold";
	desc.args_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"frames": { "type": "integer", "minimum": 1, "maximum": 1000000 },
			"fixed_dt": { "type": "number", "minimum": 0.000001, "maximum": 1 }
		},
		"required": ["frames"],
		"additionalProperties": false
	})");
	desc.result_schema = json::parse(R"({
		"type": "object",
		"properties": {
			"frame": { "type": "integer", "minimum": 0 },
			"target_frame": { "type": "integer", "minimum": 0 }
		},
		"required": ["frame", "target_frame"],
		"additionalProperties": false
	})");
	desc.invoke = [&control](const cmd::call& request) {
		const json::object& args = request.args.as_object();
		const auto frames = cmd::json_schema::integer_of(args.at("frames"));
		if (!frames || *frames < 1 || *frames > max_frames) {
			return cmd::result::failure(cmd::codes::invalid_args, "frames is not an integer in 1..1000000");
		}
		std::optional<float> fixed_dt;
		if (const json::value* dt = args.if_contains("fixed_dt")) {
			fixed_dt = static_cast<float>(dt->to_number<double>());
		}
		control.advance(static_cast<std::uint64_t>(*frames), fixed_dt);
		return cmd::result::success(json::object{
			{ "frame", static_cast<std::int64_t>(control.frame()) },
			{ "target_frame", static_cast<std::int64_t>(control.frame() + control.pending()) } });
	};
	frame_command_registrations registrations;
	registrations.advance = registry.add(std::move(desc));

	cmd::command_desc resume;
	resume.id = "app.resume";
	resume.summary = "Release the time hold of app.advance: the frame loop runs on its own time again";
	resume.args_schema = json::parse(R"({ "type": "object", "properties": {}, "additionalProperties": false })");
	resume.result_schema = json::parse(R"({
		"type": "object",
		"properties": { "frame": { "type": "integer", "minimum": 0 } },
		"required": ["frame"],
		"additionalProperties": false
	})");
	resume.invoke = [&control](const cmd::call&) {
		control.release();
		return cmd::result::success(json::object{ { "frame", static_cast<std::int64_t>(control.frame()) } });
	};
	registrations.resume = registry.add(std::move(resume));
	return registrations;
}
