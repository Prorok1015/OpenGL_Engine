#include "edt_game_view_logic.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include "edt_camera_redirect.h"
#include "scn_camera_component.hpp"
#include "scn_model.h"

namespace edt::game_view
{
	namespace {
		bool is_game_camera(const scn::camera_component& cam)
		{
			return !cam.color_targets.empty() && is_game_camera_target(cam.color_targets[0]);
		}
	}

	std::vector<camera_entry> collect_cameras(entt::registry& reg)
	{
		std::vector<camera_entry> out;
		for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
			if (!is_game_camera(cam)) continue;
			const auto* name = reg.try_get<scn::name_component>(ent);
			out.push_back({ ent, name && !name->name.empty()
				? name->name
				: std::format("Camera {}", static_cast<std::uint32_t>(entt::to_entity(ent))) });
		}
		// Views walk the storage back to front; keep the picker (and the default pick) in creation order.
		std::ranges::sort(out, {}, [](const camera_entry& e) { return entt::to_entity(e.entity); });
		return out;
	}

	void selection::refresh(const entt::registry* reg, std::vector<camera_entry> cameras)
	{
		if (reg != m_registry) {
			m_registry = reg;
			m_selected = entt::null;
		}
		m_cameras = std::move(cameras);
		const auto it = std::ranges::find(m_cameras, m_selected, &camera_entry::entity);
		if (it == m_cameras.end()) {
			m_selected = m_cameras.empty() ? entt::null : m_cameras.front().entity;
		}
	}

	void selection::select(int index)
	{
		if (index >= 0 && index < static_cast<int>(m_cameras.size())) {
			m_selected = m_cameras[static_cast<std::size_t>(index)].entity;
		}
	}

	int selection::selected_index() const
	{
		const auto it = std::ranges::find(m_cameras, m_selected, &camera_entry::entity);
		return it == m_cameras.end() ? -1 : static_cast<int>(it - m_cameras.begin());
	}

	void apply_viewports(entt::registry& reg, entt::entity selected, glm::ivec2 size)
	{
		const bool usable = size.x > 0 && size.y > 0;
		for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
			if (!is_game_camera(cam)) continue;
			cam.m_viewport.size = (ent == selected && usable) ? size : glm::ivec2{ 0 };
		}
	}

	glm::vec2 fit_letterbox(glm::vec2 avail, glm::vec2 content)
	{
		if (avail.x <= 0.f || avail.y <= 0.f || content.x <= 0.f || content.y <= 0.f) {
			return { 0.f, 0.f };
		}
		const float scale = std::min(avail.x / content.x, avail.y / content.y);
		return content * scale;
	}
}
