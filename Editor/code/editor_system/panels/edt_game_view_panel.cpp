#include "edt_game_view_panel.h"
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/gui/gui_system.h>
#include <snk/scene/scn_camera_component.hpp>
#include <imgui.h>

namespace edt
{
	game_view_panel::game_view_panel(rnd::render_system& rnd, gui::gui_system& gui)
		: panel_base("game_view", "Game view")
		, m_rnd(rnd)
		, m_gui(gui)
	{
		m_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
		m_open = false;
	}

	void game_view_panel::apply_viewports_to_all(const res::tag& target, glm::ivec2 size, entt::entity picked)
	{
		// Only the picked camera draws in its own world; the other worlds' cameras on the same target composite on top.
		entt::registry* own = m_get_registry ? m_get_registry() : nullptr;
		if (m_for_each_registry) {
			m_for_each_registry([&](entt::registry& reg) {
				game_view::apply_target_viewports(reg, target, size, &reg == own ? picked : entt::null);
			});
		} else if (own) {
			game_view::apply_target_viewports(*own, target, size, picked);
		}
	}

	void game_view_panel::on_hidden()
	{
		m_target.reset();
		apply_viewports_to_all({}, {});
	}

	void game_view_panel::on_render()
	{
		m_target.reset();
		entt::registry* reg = m_get_registry ? m_get_registry() : nullptr;
		if (!reg) {
			apply_viewports_to_all({}, {});
			m_selection.refresh(nullptr, {});
			ImGui::TextDisabled("(no active world)");
			return;
		}

		m_selection.refresh(reg, game_view::collect_cameras(*reg));
		if (m_selection.cameras().empty()) {
			apply_viewports_to_all({}, {});
			ImGui::TextDisabled("(no game camera in this world)");
			return;
		}

		const int current = m_selection.selected_index();
		ImGui::SetNextItemWidth(220.f);
		if (ImGui::BeginCombo("Camera", m_selection.cameras()[static_cast<std::size_t>(current)].name.c_str())) {
			for (int i = 0; i < static_cast<int>(m_selection.cameras().size()); ++i) {
				// Node names are not unique ("Camera" is common).
				ImGui::PushID(i);
				if (ImGui::Selectable(m_selection.cameras()[static_cast<std::size_t>(i)].name.c_str(), i == current)) {
					m_selection.select(i);
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		ImGui::Separator();

		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const entt::entity picked = m_selection.selected();
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		const auto& cam = reg->get<scn::camera_component>(picked);
		apply_viewports_to_all(cam.color_targets[0], edt::to_framebuffer_size({ avail.x, avail.y }, { fb_scale.x, fb_scale.y }), picked);

		auto& textures = m_rnd.get_texture_manager();
		const auto texture = textures.binding(textures.id_of(cam.color_targets[0])).handle;
		if (!texture || cam.m_viewport.size.x <= 0 || cam.m_viewport.size.y <= 0) {
			ImGui::TextDisabled("(waiting for the camera to render)");
			return;
		}

		m_target = cam.color_targets[0];
		const glm::vec2 fit = game_view::fit_letterbox({ avail.x, avail.y }, glm::vec2(cam.m_viewport.size));
		const ImVec2 origin = ImGui::GetCursorPos();
		ImGui::SetCursorPos({ origin.x + (avail.x - fit.x) * 0.5f, origin.y + (avail.y - fit.y) * 0.5f });
		ImTextureID tex_id = m_gui.get_backend_interface()->to_imgui(texture);
		// UV (0,1)->(1,0) flips Y for OpenGL convention
		ImGui::Image(tex_id, { fit.x, fit.y }, ImVec2(0, 1), ImVec2(1, 0));
		if (m_input_target)
			bind_item_input(m_router, m_input_target());
	}
}
