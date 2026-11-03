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

	entt::registry* game_view_panel::pick_registry(const char*& why)
	{
		entt::registry* reg = m_get_registry ? m_get_registry() : nullptr;
		if (!reg) {
			apply_viewports_to_all({}, {});
			m_selection.refresh(nullptr, {});
			why = "(no active world)";
			return nullptr;
		}
		m_selection.refresh(reg, game_view::collect_cameras(*reg));
		if (m_selection.cameras().empty()) {
			apply_viewports_to_all({}, {});
			why = "(no game camera in this world)";
			return nullptr;
		}
		return reg;
	}

	bool game_view_panel::draw_picked_camera(entt::registry& reg, glm::vec2 avail)
	{
		const entt::entity picked = m_selection.selected();
		const ImVec2 fb_scale = ImGui::GetIO().DisplayFramebufferScale;
		const auto& cam = reg.get<scn::camera_component>(picked);
		apply_viewports_to_all(cam.color_targets[0], edt::to_framebuffer_size(avail, { fb_scale.x, fb_scale.y }), picked);

		auto& textures = m_rnd.get_texture_manager();
		const auto texture = textures.binding(textures.id_of(cam.color_targets[0])).handle;
		if (!texture || cam.m_viewport.size.x <= 0 || cam.m_viewport.size.y <= 0) {
			ImGui::TextDisabled("(waiting for the camera to render)");
			return false;
		}

		m_target = cam.color_targets[0];
		const glm::vec2 fit = game_view::fit_letterbox(avail, glm::vec2(cam.m_viewport.size));
		const ImVec2 origin = ImGui::GetCursorPos();
		ImGui::SetCursorPos({ origin.x + (avail.x - fit.x) * 0.5f, origin.y + (avail.y - fit.y) * 0.5f });
		ImTextureID tex_id = m_gui.get_backend_interface()->to_imgui(texture);
		// UV (0,1)->(1,0) flips Y for OpenGL convention
		ImGui::Image(tex_id, { fit.x, fit.y }, ImVec2(0, 1), ImVec2(1, 0));
		return true;
	}

	void game_view_panel::on_render()
	{
		m_target.reset();
		const char* why = nullptr;
		entt::registry* reg = pick_registry(why);
		if (!reg) {
			ImGui::TextDisabled("%s", why);
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
		if (draw_picked_camera(*reg, { avail.x, avail.y }) && m_input_target)
			bind_item_input(m_router, m_input_target());
	}

	void game_view_panel::render_fullscreen()
	{
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(viewport->Pos);
		ImGui::SetNextWindowSize(viewport->Size);
		ImGui::SetNextWindowViewport(viewport->ID);
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus
			| ImGuiWindowFlags_NoScrollWithMouse;

		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
		// The letterbox bars of a game are black, not the editor's window colour.
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 1.f));
		const bool shown = ImGui::Begin("##game_mode_view", nullptr, flags);
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
		if (shown) {
			m_target.reset();
			const char* why = nullptr;
			if (entt::registry* reg = pick_registry(why)) {
				const ImVec2 avail = ImGui::GetContentRegionAvail();
				draw_picked_camera(*reg, { avail.x, avail.y });
			} else {
				ImGui::TextDisabled("%s", why);
			}
			// Bindings are cleared every frame; the whole window is the game's, hovered or not.
			if (m_router && m_input_target) {
				bind_game_window_input(m_router, m_input_target(), { viewport->Pos.x, viewport->Pos.y },
					{ viewport->Size.x, viewport->Size.y }, m_input_blocked && m_input_blocked());
			}
		} else {
			on_hidden();
		}
		ImGui::End();
	}
}
