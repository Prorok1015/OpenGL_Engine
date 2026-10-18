#pragma once
#include "wnd_window.h"
#include "wnd_window_context.h"
#include <array>

struct GLFWcursor;

namespace wnd
{
	class glfw_window final : public window, public gl_surface
	{
	public:
		explicit glfw_window(context ctx_);
		~glfw_window() override;

		bool is_shutdown() const override;
		void shutdown(bool close = true) override;
		void set_cursor_mode(CURSOR_MODE mode) override;
		void set_logo(const ds::fixed_vector<window_image, 2>& images) override;
		void set_title(std::string_view title) override;
		void set_cursor_shape(cursor_shape shape) override;
		std::string get_clipboard_text() const override;
		void set_clipboard_text(std::string_view text) override;

		glm::ivec2 get_backbuffer_size() const override;
		const glm::ivec2& get_size() const override { return m_size; }
		float get_content_scale() const override;
		void* get_native_handle() const override { return m_ctx.get_id(); }
		gl_surface* gl_surface_ptr() const override;

		void make_current() override;
		void swap_buffers() override;
		void set_swap_interval(int interval) override;
		void* get_proc_address(const char* name) const override;

		GLFWwindow* get_glfw_handle() const { return m_ctx.get_id(); }
		void set_size(glm::ivec2 size) { m_size = size; }
		void update_frame();
		void init_frame();

	private:
		context m_ctx;
		// Created lazily and destroyed with the window (must precede glfwTerminate).
		std::array<GLFWcursor*, static_cast<size_t>(cursor_shape::count)> m_cursors{};
		glm::ivec2 m_size{ 0 };
		double m_delta = 0.f;
		double m_last_time = 0.f;
	};
}
