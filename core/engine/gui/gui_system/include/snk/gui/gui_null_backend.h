#pragma once
#include <snk/gui/gui_backend_interface.h>
#include <snk/common/snk_api.h>

struct ImGuiContext;

namespace gui
{
	// Renderer half for a host without a display (--headless): owns the ImGui context, builds the font atlas on
	// the CPU only, draws nothing and touches no graphics API. Layout settings are neither read nor written.
	class SNK_API null_imgui_backend final : public imgui_backend_interface
	{
	public:
		explicit null_imgui_backend(rnd::driver::driver_interface& drv) : imgui_backend_interface(drv) {}

		void init() override;
		void new_frame() override;
		void render() override;
		void shutdown() override;

	private:
		ImGuiContext* m_context = nullptr;
	};
}