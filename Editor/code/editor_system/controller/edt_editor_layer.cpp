#include "edt_editor_layer.h"
#include "edt_main_menu.h"
#include <snk/editor/edt_editor_context.h>
#include <imgui.h>
#include <algorithm>
#include <format>

edt::drawn_layout edt::draw_layouts(dockspace& dock, panel_registry& panels, action_registry& actions, const editor_layouts& layouts)
{
	// Nothing to act on yet, and the person's keys must not reach the editor behind the screen.
	if (layouts.loading_active && layouts.draw_loading && layouts.loading_active()) {
		layouts.draw_loading();
		return drawn_layout::loading;
	}
	const auto game = [&layouts] { return layouts.game_active && layouts.draw_game && layouts.game_active(); };
	// Before either layout, so a shortcut also works without the menu bar; an action that switches the layout
	// takes effect in this frame. In game mode the other keys belong to the game.
	const std::function<bool(std::string_view)> any;
	if (!(layouts.modal_active && layouts.modal_active())) {
		fire_shortcuts(actions, game() ? layouts.game_shortcuts : any);
	}
	if (game()) {
		dock.keep_alive();
		layouts.draw_game();
		return drawn_layout::game;
	}
	dock.render(panels, actions);
	return drawn_layout::editor;
}

std::string edt::loading_screen_text(std::string_view level)
{
	return std::format("Loading {}...", level);
}

void edt::draw_loading_screen(std::string_view text)
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
	ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 1.f));
	const bool shown = ImGui::Begin(loading_screen_window, nullptr, flags);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar(3);
	if (shown) {
		const char* begin = text.data();
		const char* end = text.data() + text.size();
		const ImVec2 text_size = ImGui::CalcTextSize(begin, end);
		const ImVec2 window_size = ImGui::GetWindowSize();
		ImGui::SetCursorPos(ImVec2(std::max(0.f, (window_size.x - text_size.x) * 0.5f), std::max(0.f, (window_size.y - text_size.y) * 0.5f)));
		ImGui::TextUnformatted(begin, end);
	}
	ImGui::End();
}

void edt::editor_layer::on_update(std::chrono::duration<float> dt)
{
	if (m_frame_hook) m_frame_hook();
	bool game_drawn = false;
	if (m_context) {
		game_drawn = draw_layouts(m_dockspace, m_context->panels(), m_context->actions(), m_layouts) == drawn_layout::game;
	}
	// Обрабатываем legacy-callbacks (file dialogs и прочие per-frame вещи) без menu bar
	m_manager.set_show_main_menu(false);
	m_manager.process();
	// Input of the game window is decided only now: a popup an implicit callback opened this frame takes it.
	if (game_drawn && m_game_after_implicit) m_game_after_implicit();
}
