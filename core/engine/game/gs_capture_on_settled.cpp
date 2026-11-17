#include <snk/game/gs_capture_on_settled.h>
#include <snk/application/app_frame_control.h>
#include <snk/command/cmd_base64.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_variables.h>
#include <snk/project/prj_json_text.h>
#include <snk/project/prj_project.h>

#include <boost/json/object.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>

namespace {
	CFG_VAR_DEF_STRING(cfg_capture_on_settled, "capture/on_settled", "");
	CFG_VAR_DEF_STRING(cfg_capture_viewport, "capture/viewport", "");
	CFG_VAR_DEF_INT(cfg_capture_delay_frames, "capture/delay_frames", 30);

	// A view that has no render target yet answers `failed`; ask again for this many frames before giving up.
	constexpr int max_attempts = 120;

	struct capture_state
	{
		bool done = false;
		int attempts = 0;
	};

	bool is_inside(const std::filesystem::path& file, const std::filesystem::path& root)
	{
		auto at = file.begin();
		for (const auto& part : root) {
			if (part.empty()) continue;
			if (at == file.end() || *at != part) return false;
			++at;
		}
		return true;
	}
}

std::string gs::write_capture(const std::filesystem::path& file, std::string_view png, const std::filesystem::path& project_root)
{
	std::error_code error;
	const std::filesystem::path given = std::filesystem::absolute(file, error);
	if (error || !given.has_filename() || given.filename() == "." || given.filename() == "..") {
		return std::format("'{}' is no file name", file.string());
	}
	// The folder canonical, the name as given: a link at the file is refused by the writer, never followed here.
	const std::filesystem::path target = std::filesystem::weakly_canonical(given.parent_path(), error) / given.filename();
	if (error) {
		return std::format("'{}': its folder cannot be resolved", file.string());
	}
	if (!project_root.empty()) {
		const std::filesystem::path root = std::filesystem::weakly_canonical(project_root, error);
		if (error || is_inside(target, root)) {
			return std::format("'{}' is inside the project: capture/on_settled writes outside it", file.string());
		}
	}
	if (const auto problem = prj::write_project_file(target.parent_path(), target.filename(), png)) {
		return *problem;
	}
	return {};
}

void gs::register_capture_on_settled(app::frame_stage_registry& registry)
{
	const std::string path = *cfg_capture_on_settled;
	if (path.empty()) {
		return;
	}
	const std::string viewport = *cfg_capture_viewport;
	const auto delay = static_cast<std::uint64_t>(std::max(0, static_cast<int>(cfg_capture_delay_frames)));
	auto state = std::make_shared<capture_state>();
	// Registered after the `tasks` stage, which is also after `input`: the previous frame is complete and the next
	// one has not started to render.
	registry.add_stage("capture_on_settled", app::builtin_stage::input, app::stage_side::after, [=](app::stage_context& c) {
		if (state->done) {
			return;
		}
		const auto& control = c.storage.require<app::frame_control>();
		const auto settled = control.settled_frame();
		if (!settled || control.frame() < *settled + delay) {
			return;
		}

		boost::json::object args;
		if (!viewport.empty()) {
			args["viewport"] = viewport;
		}
		const cmd::result result = c.storage.require<cmd::command_registry>().invoke("viewport.capture", args, cmd::origin::test());
		if (!result.ok) {
			if (++state->attempts >= max_attempts) {
				state->done = true;
				egLOG_ERROR("capture", "capture/on_settled failed: {} ({})", result.error, result.code);
			}
			return;
		}
		state->done = true;
		const boost::json::object& value = result.value.as_object();
		const auto png = cmd::base64::decode(std::string(value.at("png_base64").as_string()));
		if (!png) {
			egLOG_ERROR("capture", "capture/on_settled: the capture is no PNG");
			return;
		}
		const std::filesystem::path root = c.storage.has_value<prj::project_context>() ? c.storage.require<prj::project_context>().root
			: std::filesystem::path{};
		const std::string_view bytes(reinterpret_cast<const char*>(png->data()), png->size());
		if (std::string problem = gs::write_capture(path, bytes, root); !problem.empty()) {
			egLOG_WARN("capture", "capture/on_settled: not written: {}", problem);
			return;
		}
		// run_headless.sh --wait-for-log waits for this line.
		egLOG_INFO("capture", "captured viewport '{}' ({}x{}) to {}", std::string(value.at("viewport").as_string()),
			value.at("width").as_int64(), value.at("height").as_int64(), path);
	});
}
