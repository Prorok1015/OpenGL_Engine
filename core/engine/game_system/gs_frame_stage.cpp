#include "gs_frame_stage.h"
#include <algorithm>
#include "wnd_window_system.h"
#include "level/scn_level_manager.h"
#include "rnd_frame_assembler.h"
#include "rnd_render_system.h"
#include "eng_profiler.h"
#include "mem_allocator.h"

void gs::frame_stage_registry::add_stage(int order, stage_fn fn)
{
	m_stages.push_back({ order, std::move(fn) });
	m_sorted = false;
}

std::vector<int> gs::frame_stage_registry::stage_orders() const
{
	std::vector<int> orders;
	orders.reserve(m_stages.size());
	for (const auto& stage : m_stages) {
		orders.push_back(stage.order);
	}
	std::stable_sort(orders.begin(), orders.end());
	return orders;
}

void gs::frame_stage_registry::run(frame_step_context& ctx)
{
	if (!m_sorted) {
		std::stable_sort(m_stages.begin(), m_stages.end(),
			[](const entry& a, const entry& b) { return a.order < b.order; });
		m_sorted = true;
	}
	for (auto& stage : m_stages) {
		stage.fn(ctx);
	}
}

void gs::register_core_frame_stages(frame_stage_registry& registry)
{
	// Must run before any stage allocates from the frame allocator; shared so every loop gets it.
	registry.add_stage(frame_begin_order, [](frame_step_context&) {
		ds::frame_allocator_reset();
	});
	registry.add_stage(10, [](frame_step_context& c) {
		PROFILE_SCOPE("PollEvents");
		c.storage.require<wnd::window_system>().pool_events();
	});
	// Inject the render managers into the frame_context before extract(30)/render(40) read them
	// (EPIC-49 US-49-6).
	registry.add_stage(28, [](frame_step_context& c) {
		c.storage.require<rnd::render_system>().begin_frame(c.render);
	});
	registry.add_stage(20, [](frame_step_context& c) {
		c.storage.require<scn::level_manager>().update(c.dt);
	});
	registry.add_stage(30, [](frame_step_context& c) {
		c.storage.require<rnd::frame_assembler>().build_frame(c.render);
	});
	registry.add_stage(40, [](frame_step_context& c) {
		c.storage.require<rnd::render_system>().render_frame(c.render);
	});
	registry.add_stage(50, [](frame_step_context& c) {
		PROFILE_SCOPE("SwapBuffers");
		c.storage.require<wnd::window_system>().process_windows();
	});
}
