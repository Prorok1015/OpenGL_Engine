#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/common/snk_api.h>
#include <snk/render/rnd_capture_service.h>

namespace rnd
{
	// Registers `viewport.capture{viewport?, width?, height?}` -> `{viewport, width, height, format:"png", png_base64}`.
	// `viewport` is an id of the service ("scene", "game", ...; absent: the first registered one), `width` and
	// `height` scale the picture (one of them keeps the aspect). Failures: `not_found` for an unknown viewport,
	// `failed` when the view has no render target yet or the driver cannot read it (the null backend answers with
	// a placeholder picture, a GL one with the real frame).
	// `service` must outlive the registration.
	[[nodiscard]] SNK_API cmd::registration register_capture_commands(cmd::command_registry& registry, capture_service& service);
}
