#include "edt_game_view_logic.h"
#include <algorithm>
#include <cstdint>
#include <format>
#include "edt_camera_redirect.h"
#include <snk/scene/scn_camera_component.hpp>
#include <snk/scene/scn_model.h>
#include <snk/scene/level/scn_node_index.h>

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
			const auto* node = reg.try_get<scn::node_id_component>(ent);
			out.push_back({ ent, name && !name->name.empty()
				? name->name
				: std::format("Camera {}", static_cast<std::uint32_t>(entt::to_entity(ent))),
				node ? node->path : std::vector<std::uint64_t>{} });
		}
		// Views walk the storage back to front; keep the picker (and the default pick) in creation order.
		std::ranges::sort(out, {}, [](const camera_entry& e) { return entt::to_entity(e.entity); });
		return out;
	}

	void selection::refresh(entt::registry* reg, std::vector<camera_entry> cameras, std::string_view world)
	{
		m_cameras = std::move(cameras);
		if (!reg) {
			// No world on show: the pick and its node wait for the world (the same one made again, or another).
			m_token = 0;
			m_selected = entt::null;
			return;
		}
		if (world != m_world) {
			m_world = std::string{ world };
			m_pick_path.clear();
			m_selected = entt::null;
		}
		const std::uint64_t token = registry_token(*reg);
		if (token != m_token) {
			// A registry this selection has not seen: whatever its entity ids were in the last one, they mean nothing here.
			m_token = token;
			m_selected = entt::null;
		}
		auto it = std::ranges::find(m_cameras, m_selected, &camera_entry::entity);
		if (it == m_cameras.end() && !m_pick_path.empty()) {
			it = std::ranges::find(m_cameras, m_pick_path, &camera_entry::path);
		}
		if (it == m_cameras.end() && !m_cameras.empty()) it = m_cameras.begin();
		m_selected = it == m_cameras.end() ? entt::null : it->entity;
		remember_path();
	}

	void selection::forget()
	{
		remember_path();
		m_token = 0;
		m_cameras.clear();
		m_selected = entt::null;
	}

	void selection::remember_path()
	{
		const auto it = std::ranges::find(m_cameras, m_selected, &camera_entry::entity);
		// A camera no desc spawned has no node: the path of the pick before it stays until another pick replaces it.
		if (it != m_cameras.end() && !it->path.empty()) m_pick_path = it->path;
		else if (it != m_cameras.end()) m_pick_path.clear();
	}

	void selection::select(int index)
	{
		if (index >= 0 && index < static_cast<int>(m_cameras.size())) {
			m_selected = m_cameras[static_cast<std::size_t>(index)].entity;
			remember_path();
		}
	}

	int selection::selected_index() const
	{
		const auto it = std::ranges::find(m_cameras, m_selected, &camera_entry::entity);
		return it == m_cameras.end() ? -1 : static_cast<int>(it - m_cameras.begin());
	}

	void apply_target_viewports(entt::registry& reg, const res::tag& target, glm::ivec2 size, entt::entity only)
	{
		const bool usable = target.is_valid() && size.x > 0 && size.y > 0;
		for (auto [ent, cam] : reg.view<scn::camera_component>().each()) {
			if (!is_game_camera(cam)) continue;
			const bool allowed = only == entt::null || ent == only;
			cam.m_viewport.size = (usable && allowed && cam.color_targets[0] == target) ? size : glm::ivec2{ 0 };
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
