#pragma once
#include <functional>
#include <vector>
#include <chrono>
#include "ds/ds_store.hpp"

namespace rnd { struct frame_context; }

namespace gs
{
	// Per-frame data threaded to every frame stage (EPIC-49 US-49-8). `render` is the single
	// frame_context shared by the extract and render stages within one frame.
	struct frame_step_context
	{
		ds::app_data_storage& storage;
		std::chrono::duration<float> dt;
		rnd::frame_context& render;
	};

	// Ordered registry of per-frame stages, replacing the hardcoded god on_step. A service adds
	// its stage with an explicit order key; run() executes them in stable order. This mirrors how
	// render passes / extractors self-register, but with an explicit cross-service ordering.
	class frame_stage_registry
	{
	public:
		using stage_fn = std::function<void(frame_step_context&)>;

		void add_stage(int order, stage_fn fn);
		void run(frame_step_context& ctx);

		// Registered order keys, ascending (introspection for tests).
		[[nodiscard]] std::vector<int> stage_orders() const;

	private:
		struct entry { int order; stage_fn fn; };
		std::vector<entry> m_stages;
		bool m_sorted = false;
	};

	// Order key of the frame-begin stage (frame allocator reset); runs before everything else.
	inline constexpr int frame_begin_order = 5;

	// Stages common to every frame loop, in canonical order:
	// frame_begin(5) -> poll_events(10) -> level update(20) -> extract(30) -> render(40) -> present(50).
	void register_core_frame_stages(frame_stage_registry& registry);
}
