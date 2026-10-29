#include <snk/gui/gui_null_backend.h>
#include <imgui.h>

void gui::null_imgui_backend::init()
{
	IMGUI_CHECKVERSION();
	m_context = ImGui::CreateContext();
	ImGui::SetCurrentContext(m_context);
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
	io.IniFilename = nullptr;
	io.BackendRendererName = "snk_null";
	ImGui::StyleColorsDark();
}

void gui::null_imgui_backend::new_frame()
{
	ImGuiIO& io = ImGui::GetIO();
	if (!io.Fonts->IsBuilt()) {
		// NewFrame needs a built atlas; a renderer would upload these pixels, here they stay in memory.
		unsigned char* pixels = nullptr;
		int width = 0;
		int height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
	}
}

void gui::null_imgui_backend::render()
{
	ImGui::Render();
}

void gui::null_imgui_backend::shutdown()
{
	if (!m_context) {
		return;
	}
	ImGui::DestroyContext(m_context);
	m_context = nullptr;
}
