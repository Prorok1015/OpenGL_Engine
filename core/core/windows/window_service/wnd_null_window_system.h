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
		void set_cursor_shape(cursor_shape shape) override { m_cursor_shape = shape; }
		std::string get_clipboard_text() const override { return m_clipboard; }
		void set_clipboard_text(std::string_view text) override { m_clipboard = text; }

		glm::ivec2 get_backbuffer_size() const override { return m_size; }
		const glm::ivec2& get_size() const override { return m_size; }
		float get_content_scale() const override { return 1.f; }
		void* get_native_handle() const override { return nullptr; }

		const std::string& get_title() const { return m_title; }
		CURSOR_MODE get_cursor_mode() const { return m_cursor_mode; }
		cursor_shape get_cursor_shape() const { return m_cursor_shape; }

	private:
		glm::ivec2 m_size;
		std::string m_title;
		std::string m_clipboard;
		CURSOR_MODE m_cursor_mode = CURSOR_MODE::NORMAL;
		cursor_shape m_cursor_shape = cursor_shape::arrow;
		bool m_should_close = false;
	};

	class SNK_API null_window_system final : public window_system
	{
	public:
		null_window_system();
		~null_window_system() override;

		void init() override {}

		std::shared_ptr<window> get_active_window() override { return m_window; }
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
