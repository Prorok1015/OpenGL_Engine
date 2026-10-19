#include <snk/game/gs_capture_on_settled.h>
#include <snk/application/app_frame_control.h>
#include <snk/command/cmd_base64.h>
#include <snk/command/cmd_registry.h>
#include <snk/common/logger/engine_log.h>
#include <snk/config/cfg_api.h>

#include <boost/json/object.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
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
		std::ofstream file(path, std::ios::binary);
		if (!png || !file.write(reinterpret_cast<const char*>(png->data()), static_cast<std::streamsize>(png->size()))) {
			egLOG_ERROR("capture", "capture/on_settled: cannot write {}", path);
			return;
		}
		// run_headless.sh --wait-for-log waits for this line.
		egLOG_INFO("capture", "captured viewport '{}' ({}x{}) to {}", std::string(value.at("viewport").as_string()),
			value.at("width").as_int64(), value.at("height").as_int64(), path);
	});
}
