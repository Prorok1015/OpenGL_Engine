#pragma once
#include <snk/command/cmd_registry.h>
#include <snk/common/snk_api.h>
#include <snk/render/rnd_capture_service.h>

namespace res { class resource_system; }

namespace rnd
{
	// The error code of capture.compare when the capture and the reference differ in size.
	inline constexpr const char* capture_size_mismatch = "size_mismatch";

	// Registers `viewport.capture{viewport?, width?, height?}` -> `{viewport, width, height, format:"png", png_base64}`.
	// `viewport` is an id of the service ("scene", "game", ...; absent: the first registered one), `width` and
	// `height` scale the picture (one of them keeps the aspect). Failures: `not_found` for an unknown viewport,
	// `invalid_args` for a size over the limits (see capture_service::max_side and max_pixels, the derived side
	// of a one-sided request counts), `failed` when the target is too big to read
	// back, the view has no render target yet or the driver cannot read it (the null backend answers with a
	// placeholder picture, a GL one with the real frame).
	// `service` must outlive the registration.
	[[nodiscard]] SNK_API cmd::registration register_capture_commands(cmd::command_registry& registry, capture_service& service);

	// Registers `capture.compare{reference, viewport?, fuzz?, max_diff_pct?}` -> `{match, diff_pixels, total_pixels,
	// diff_pct, width, height, viewport, reference}`. `reference` is a res:// picture (png, jpg) loaded through
	// `resources`; `viewport` defaults to "game". A pixel differs when a channel differs by more than `fuzz` % of 255
	// (default 4), `match` is diff_pct <= `max_diff_pct` (default 2). The capture is compared at the size of the
	// target and opaque, as viewport.capture saves it. Failures: `invalid_args` for a reference that is not a
	// relative res:// path or has more than capture_service::max_pixels pixels, `not_found` for an unknown viewport,
	// `failed` for a reference that cannot be loaded or a capture that fails, `size_mismatch` when the sizes differ.
	// `service` and `resources` must outlive the registration.
	[[nodiscard]] SNK_API cmd::registration register_capture_compare_command(cmd::command_registry& registry, capture_service& service,
		res::resource_system& resources);
}
