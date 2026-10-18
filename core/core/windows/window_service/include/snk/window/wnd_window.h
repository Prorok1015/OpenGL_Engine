#pragma once
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include "common.h"
#include <cstdint>
#include <string>
#include <string_view>
#include "ds/ds_fixed_vector.hpp"
#include "snk_api.h"
#include <snk/window/wnd_gl_surface.h>

namespace wnd
{
	enum class CURSOR_MODE
	{
		DISABLE,
		NORMAL,
		HIDDEN
	};

	enum class cursor_shape
	{
		arrow,
		text_input,
		resize_all,
		resize_ns,
		resize_ew,
		resize_nesw,
		resize_nwse,
		hand,
		not_allowed,
		count
	};

	// Client API the window is created for; a GL context can only be attached by glfwCreateWindow.
	enum class graphics_api
	{
		opengl,
		none
	};

	struct window_image
	{
		enum class TYPE : uint8_t
		{
			ICON,
			LOGO
		};

		unsigned char* pixels = nullptr;
		int width = 0;
		int height = 0;
		TYPE type = TYPE::LOGO;
	};

	// Backend-agnostic window: concrete implementations live behind this interface.
	class SNK_API window
	{
	public:
		virtual ~window();

		virtual bool is_shutdown() const = 0;
		virtual void shutdown(bool close = true) = 0;
		virtual void set_cursor_mode(CURSOR_MODE mode) = 0;
		virtual void set_logo(const ds::fixed_vector<window_image, 2>& images) = 0;
		virtual void set_title(std::string_view title) = 0;
		virtual void set_cursor_shape(cursor_shape shape) = 0;
		virtual std::string get_clipboard_text() const = 0;
		virtual void set_clipboard_text(std::string_view text) = 0;

		virtual glm::ivec2 get_backbuffer_size() const = 0;
		virtual const glm::ivec2& get_size() const = 0;
		virtual float get_content_scale() const = 0;
		// GL context of this window; nullptr when it has none (null window, graphics_api::none).
		virtual gl_surface* gl_surface_ptr() const { return nullptr; }

		float aspect_ratio() const { if (is_minimize_mode()) return 0.f; return (float)get_size().x / (float)get_size().y; }
		bool is_minimize_mode() const { return get_size() == glm::zero<glm::ivec2>(); }
	};
}
