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
			// Entity index only (no version bits): a recycled slot reuses its texture.
			const std::string path = std::format("__game_camera_rt_{}_{}", scope, static_cast<std::uint32_t>(entt::to_entity(ent)));
			const res::tag tag(res::tag::memory, path);
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

void edt::camera_redirector::update(scn::level& level, const release_fn& release)
{
	begin_frame();
	level.for_each_world([this](scn::world& world) {
		redirect(world.state(), world.get_name());
	});
	end_frame(release);
}
