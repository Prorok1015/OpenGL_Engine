#include "gs_frame_stage.h"
#include "engine_log.h"
#include "wnd_window_system.h"
#include "level/scn_level_manager.h"
#include "inp_ecs_input_manager.h"
#include "rnd_frame_assembler.h"
#include "rnd_render_system.h"
#include "eng_profiler.h"
#include "mem_allocator.h"

std::optional<gs::builtin_stage> gs::parse_builtin_stage(std::string_view name) noexcept
{
	for (std::size_t i = 0; i < builtin_stage_count; ++i) {
		const auto stage = static_cast<builtin_stage>(i);
		if (to_string(stage) == name) {
			return stage;
		}
	}
	return std::nullopt;
}

bool gs::frame_stage_registry::set_body(builtin_stage stage, stage_fn fn)
{
	auto& slot = m_bodies[static_cast<std::size_t>(stage)];
	if (slot) {
		egLOG_ERROR("app", "frame stage '{}' already has a body", to_string(stage));
		return false;
	}
	slot = std::move(fn);
	return true;
}

bool gs::frame_stage_registry::add_stage(std::string name, builtin_stage anchor, stage_side side, stage_fn fn)
{
	if (name.empty() || parse_builtin_stage(name)) {
		egLOG_ERROR("app", "frame stage name '{}' is empty or built-in", name);
		return false;
	}
	if (anchor == builtin_stage::begin && side == stage_side::before) {
		egLOG_ERROR("app", "frame stage '{}' cannot run before 'begin'", name);
		return false;
	}
	for (const auto& stage : m_extra) {
		if (stage.name == name) {
			egLOG_ERROR("app", "frame stage '{}' is already registered", name);
			return false;
		}
	}
	m_extra.push_back({ std::move(name), anchor, side, std::move(fn) });
	return true;
}

void gs::frame_stage_registry::run(frame_step_context& ctx)
{
	const auto run_extra = [&](builtin_stage anchor, stage_side side) {
		for (auto& stage : m_extra) {
			if (stage.anchor == anchor && stage.side == side) {
				stage.fn(ctx);
			}
		}
	};
	for (std::size_t i = 0; i < builtin_stage_count; ++i) {
		const auto stage = static_cast<builtin_stage>(i);
		run_extra(stage, stage_side::before);
		if (m_bodies[i]) {
			m_bodies[i](ctx);
		}
		run_extra(stage, stage_side::after);
	}
}

std::vector<std::string> gs::frame_stage_registry::stage_names() const
{
	std::vector<std::string> names;
	names.reserve(builtin_stage_count + m_extra.size());
	const auto add_extra = [&](builtin_stage anchor, stage_side side) {
		for (const auto& stage : m_extra) {
			if (stage.anchor == anchor && stage.side == side) {
				names.push_back(stage.name);
			}
		}
	};
	for (std::size_t i = 0; i < builtin_stage_count; ++i) {
		const auto stage = static_cast<builtin_stage>(i);
		add_extra(stage, stage_side::before);
		names.emplace_back(to_string(stage));
		add_extra(stage, stage_side::after);
	}
	return names;
}

void gs::register_core_frame_stages(frame_stage_registry& registry)
{
	// Must run before any stage allocates from the frame allocator; shared so every loop gets it.
	registry.set_body(builtin_stage::begin, [](frame_step_context&) {
		ds::frame_allocator_reset();
	});
	registry.set_body(builtin_stage::input, [](frame_step_context& c) {
		PROFILE_SCOPE("PollEvents");
		c.storage.require<wnd::window_system>().pool_events();
	});
	registry.set_body(builtin_stage::update, [](frame_step_context& c) {
		c.storage.require<scn::level_manager>().update(c.dt);
	});
	registry.set_body(builtin_stage::post_update, [](frame_step_context& c) {
		if (auto ecs_input = c.storage.require_shared<inp::ecs_input_manager>()) {
			ecs_input->reset_frame_deltas();
		}
	});
	// begin_frame injects the render managers into the frame_context before the extractors read
	// them (EPIC-49 US-49-6).
	registry.set_body(builtin_stage::extract, [](frame_step_context& c) {
		c.storage.require<rnd::render_system>().begin_frame(c.render);
		c.storage.require<rnd::frame_assembler>().build_frame(c.render);
	});
	registry.set_body(builtin_stage::render, [](frame_step_context& c) {
		c.storage.require<rnd::render_system>().render_frame(c.render);
	});
	registry.set_body(builtin_stage::present, [](frame_step_context& c) {
		PROFILE_SCOPE("SwapBuffers");
		c.storage.require<wnd::window_system>().process_windows();
	});
}
