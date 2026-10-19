#pragma once

namespace app
{
	// Opt-in deterministic frame time for headless runs (config `app/fixed_frame_dt`,
	// `app/freeze_after_frames`). Default-constructed: wall-clock time, nothing changes.
	struct deterministic_time
	{
		// > 0: every frame advances by exactly this many seconds (loop dt and ImGui DeltaTime).
		float fixed_dt = 0.f;
		// > 0 together with fixed_dt: after this many frames the loop dt becomes 0, so the scene
		// holds one pose and a screenshot does not depend on how many frames the runner managed.
		int freeze_after_frames = 0;

		[[nodiscard]] bool enabled() const { return fixed_dt > 0.f; }
	};
}
