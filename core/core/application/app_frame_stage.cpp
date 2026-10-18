#include <snk/application/app_frame_stage.h>
#include <algorithm>
#include <exception>
#include <snk/common/logger/engine_log.h>
#include <snk/common/mem_allocator.h>

std::optional<app::builtin_stage> app::parse_builtin_stage(std::string_view name) noexcept
{
	for (std::size_t i = 0; i < builtin_stage_count; ++i) {
		const auto stage = static_cast<builtin_stage>(i);
		if (to_string(stage) == name) {
			return stage;
		}
	}
	return std::nullopt;
}

bool app::frame_stage_registry::set_body(builtin_stage stage, stage_fn fn)
{
	auto& slot = m_bodies[static_cast<std::size_t>(stage)];
	if (slot) {
		egLOG_ERROR("app", "frame stage '{}' already has a body", to_string(stage));
		return false;
	}
	slot = std::move(fn);
	return true;
}

void app::frame_stage_registry::clear_body(builtin_stage stage)
{
	m_bodies[static_cast<std::size_t>(stage)] = nullptr;
}

bool app::frame_stage_registry::add_stage(std::string name, builtin_stage anchor, stage_side side, stage_fn fn)
{
	if (name.empty() || parse_builtin_stage(name)) {
		egLOG_ERROR("app", "frame stage name '{}' is empty or built-in", name);
		return false;
	}
	if (anchor == builtin_stage::begin && side == stage_side::before) {
		egLOG_ERROR("app", "frame stage '{}' cannot run before 'begin'", name);
		return false;
	}
	const bool taken = std::ranges::any_of(m_extra, [&](const extra_stage& s) { return s.name == name; });
	if (taken) {
		egLOG_ERROR("app", "frame stage '{}' is already registered", name);
		return false;
	}
	m_extra.push_back({ std::move(name), anchor, side, std::move(fn) });
	return true;
}

bool app::frame_stage_registry::remove_stage(std::string_view name)
{
	const auto it = std::ranges::find_if(m_extra, [&](const extra_stage& s) { return s.name == name; });
	if (it == m_extra.end()) {
		return false;
	}
	m_extra.erase(it);
	return true;
}

void app::frame_stage_registry::run(stage_context& ctx)
{
	// A stage may add or remove stages (or set bodies) while the frame runs; iterate a copy of
	// this frame's plan so such changes only take effect from the next frame.
	std::vector<stage_fn> plan;
	plan.reserve(builtin_stage_count + m_extra.size());
	const auto plan_extra = [&](builtin_stage anchor, stage_side side) {
		for (const auto& stage : m_extra) {
			if (stage.anchor == anchor && stage.side == side) {
				plan.push_back(stage.fn);
			}
		}
	};
	for (std::size_t i = 0; i < builtin_stage_count; ++i) {
		const auto stage = static_cast<builtin_stage>(i);
		plan_extra(stage, stage_side::before);
		if (m_bodies[i]) {
			plan.push_back(m_bodies[i]);
		}
		plan_extra(stage, stage_side::after);
	}
	for (auto& fn : plan) {
		fn(ctx);
	}
}

std::vector<std::string> app::frame_stage_registry::stage_names() const
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

void app::frame_stage_registry::run_init_hooks(ds::app_data_storage& storage)
{
	for (auto& hook : m_init_hooks) {
		hook(storage);
	}
}

void app::frame_stage_registry::run_term_hooks(ds::app_data_storage& storage)
{
	// One failing hook must not skip the others; the first failure is rethrown afterwards.
	std::exception_ptr first_error;
	for (auto it = m_term_hooks.rbegin(); it != m_term_hooks.rend(); ++it) {
		try {
			(*it)(storage);
		} catch (const std::exception& e) {
			egLOG_ERROR("app", "frame term hook failed: {}", e.what());
			if (!first_error) first_error = std::current_exception();
		} catch (...) {
			egLOG_ERROR("app", "frame term hook failed with an unknown exception");
			if (!first_error) first_error = std::current_exception();
		}
	}
	if (first_error) {
		std::rethrow_exception(first_error);
	}
}

void app::register_core_frame_stages(frame_stage_registry& registry)
{
	// Must run before any stage allocates from the frame allocator.
	registry.set_body(builtin_stage::begin, [](stage_context&) {
		ds::frame_allocator_reset();
	});
}
