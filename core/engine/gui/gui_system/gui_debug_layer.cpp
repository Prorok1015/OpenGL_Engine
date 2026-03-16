#include "gui_debug_layer.h"
#include "imgui.h"

void gui::debug_layer::on_update(std::chrono::duration<float> dt)
{
	m_manager.process();
}

bool gui::debug_layer::show_stats()
{
	bool is_open = true;
	//ImGui::SetNextWindowSize(ImVec2{ 400, 60 }, ImGuiCond_FirstUseEver);
	//ImGui::SetNextWindowPos(ImVec2{ 0, 20 }, ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Common stats", &is_open)) {
		ImGuiIO& io = ImGui::GetIO();
		ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
	}
	ImGui::End();
	return is_open;
}

bool gui::debug_layer::show_demo()
{
	bool show_demo_window = true;
	ImGui::ShowDemoWindow(&show_demo_window);
	return show_demo_window;
}
