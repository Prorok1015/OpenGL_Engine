#include "edt_camera_redirect.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <entt/entt.hpp>
#include <snk/scene/scn_camera_component.hpp>
#include <snk/render/rnd_render_packet.hpp>

namespace {
	constexpr std::string_view target_prefix = "memory://__game_camera_rt_";

	// The target name of a backbuffer camera: one per scope, so the cameras of a scope (the worlds of a set) that
	// draw into the same viewport draw into the same texture, in the order of their layers. A camera with a
	// viewport of its own gets a target of that viewport.
	std::string shared_target_name(std::string_view scope, const scn::viewport& viewport)
	{
		if (viewport.size.x >= 1 && viewport.size.y >= 1) {
			return std::format("__game_camera_rt_{}_{}_{}_{}_{}", scope,
				viewport.lefttop.x, viewport.lefttop.y, viewport.size.x, viewport.size.y);
		}
		return std::format("__game_camera_rt_{}", scope);
	}
}

bool edt::is_game_camera_target(const res::tag& target)
{
	return target.view().starts_with(target_prefix);
}

void edt::camera_redirector::begin_frame()
{
	m_live.clear();
}

std::size_t edt::camera_redirector::redirect(entt::registry& reg, std::string_view scope)
{
	std::size_t redirected = 0;
	for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
		if (rnd::targets_backbuffer(cam.color_targets)) {
			const res::tag tag(res::tag::memory, shared_target_name(scope, cam.m_viewport));
			if (cam.color_targets.empty()) {
				cam.color_targets.push_back(tag);
			} else {
				cam.color_targets[0] = tag;
			}
			++redirected;
		}
		const res::tag& target = cam.color_targets[0];
		if (target.view().starts_with(target_prefix) && std::ranges::find(m_live, target) == m_live.end()) {
			m_live.push_back(target);
		}
	}
	return redirected;
}

void edt::camera_redirector::end_frame(const release_fn& release)
{
	for (const auto& tag : m_known) {
		if (std::ranges::find(m_live, tag) == m_live.end() && release) {
			release(tag);
		}
	}
	m_known = m_live;
}

void edt::camera_redirector::update(scn::context_manager& contexts, const release_fn& release)
{
	begin_frame();
	contexts.for_each_set([this](scn::context_set& set) {
		set.for_each([this, &set](scn::world_context& context) {
			// The scope is the set: the worlds of a set are layers of one picture (WORLD_MESSAGING section 7).
			redirect(context.state(), set.name());
		});
	});
	end_frame(release);
}
