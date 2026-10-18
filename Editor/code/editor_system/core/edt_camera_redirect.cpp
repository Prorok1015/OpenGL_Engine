#include "edt_camera_redirect.h"
#include <format>
#include <string>
#include <entt/entt.hpp>
#include "level/scn_level.h"
#include "scn_camera_component.hpp"
#include "rnd_render_packet.hpp"
#include "res_tag.h"

std::size_t edt::redirect_backbuffer_cameras(entt::registry& reg, std::string_view scope)
{
	std::size_t redirected = 0;
	for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
		if (!rnd::targets_backbuffer(cam.color_targets)) {
			continue;
		}
		const std::string path = std::format("__game_camera_rt_{}_{}", scope, static_cast<std::uint32_t>(entt::to_integral(ent)));
		if (cam.color_targets.empty()) {
			cam.color_targets.push_back(res::tag(res::tag::memory, path));
		} else {
			cam.color_targets[0] = res::tag(res::tag::memory, path);
		}
		++redirected;
	}
	return redirected;
}

void edt::redirect_backbuffer_cameras(scn::level& level)
{
	level.for_each_world([](scn::world& world) {
		redirect_backbuffer_cameras(world.state(), world.get_name());
	});
}
