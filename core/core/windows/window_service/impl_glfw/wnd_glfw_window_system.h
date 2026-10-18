#pragma once
#include "wnd_window_system.h"
#include "wnd_glfw_window.h"
#include <unordered_map>

namespace wnd
{
	class glfw_window_system final : public window_system
	{
	public:
		glfw_window_system();
		~glfw_window_system() override;

		void init() override;

		std::shared_ptr<window> get_active_window() override;
		rnd::driver::render_context_interface* get_context() const override { return m_context.get(); }
		void* get_active_native_handle() const override { return m_active_window; }
		float get_active_content_scale() const override;
		bool is_stop_running() override;

		void pool_events() const override;
		void init_windows_frame_time() const override;
		void process_windows() const override;

		std::shared_ptr<window> find_window(GLFWwindow* handle) const;

	private:
		std::shared_ptr<glfw_window> make_window();

	private:
		std::unordered_map<GLFWwindow*, std::shared_ptr<glfw_window>> m_windows_list;
		std::unique_ptr<rnd::driver::render_context_interface> m_context = nullptr;

		GLFWwindow* m_active_window = nullptr;
		wnd::context::header m_title;
	};
}
