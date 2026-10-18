#pragma once
#include "wnd_window_system.h"
#include <memory>
#include <string>

namespace wnd
{
	// Window without a display or GL: lets window-dependent composition run in unit tests.
	class SNK_API null_window final : public window
	{
	public:
		explicit null_window(glm::ivec2 size = glm::ivec2{ 1920, 1080 }) : m_size(size) {}

		bool is_shutdown() const override { return m_should_close; }
		void shutdown(bool close = true) override { m_should_close = close; }
		void set_cursor_mode(CURSOR_MODE mode) override { m_cursor_mode = mode; }
		void set_logo(const ds::fixed_vector<window_image, 2>&) override {}
		void set_title(std::string_view title) override;
		void set_vsync(bool v) override { m_vsync = v; }

		glm::ivec2 get_backbuffer_size() const override { return m_size; }
		const glm::ivec2& get_size() const override { return m_size; }
		float get_content_scale() const override { return 1.f; }
		void* get_native_handle() const override { return nullptr; }

		const std::string& get_title() const { return m_title; }
		CURSOR_MODE get_cursor_mode() const { return m_cursor_mode; }
		bool get_vsync() const { return m_vsync; }

	private:
		glm::ivec2 m_size;
		std::string m_title;
		CURSOR_MODE m_cursor_mode = CURSOR_MODE::NORMAL;
		bool m_should_close = false;
		bool m_vsync = false;
	};

	class SNK_API null_window_system final : public window_system
	{
	public:
		null_window_system();
		~null_window_system() override;

		void init() override {}

		std::shared_ptr<window> get_active_window() override { return m_window; }
		rnd::driver::render_context_interface* get_context() const override { return nullptr; }
		void* get_active_native_handle() const override { return nullptr; }
		float get_active_content_scale() const override { return 1.f; }
		bool is_stop_running() override { return m_window->is_shutdown(); }

		void pool_events() const override {}
		void init_windows_frame_time() const override {}
		void process_windows() const override {}

	private:
		std::shared_ptr<null_window> m_window;
	};
}
