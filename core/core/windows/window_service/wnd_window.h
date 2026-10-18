#pragma once
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include "common.h"
#include <cstdint>
#include <string_view>
#include "ds/ds_fixed_vector.hpp"
#include "snk_api.h"

namespace wnd
{
	enum class CURSOR_MODE
	{
		DISABLE,
		NORMAL,
		HIDDEN
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
		virtual void set_vsync(bool v) = 0;

		virtual glm::ivec2 get_backbuffer_size() const = 0;
		virtual const glm::ivec2& get_size() const = 0;
		virtual float get_content_scale() const = 0;
		// Opaque backend handle for the GUI platform layer; nullptr when there is none.
		virtual void* get_native_handle() const = 0;

		float aspect_ratio() const { if (is_minimize_mode()) return 0.f; return (float)get_size().x / (float)get_size().y; }
		bool is_minimize_mode() const { return get_size() == glm::zero<glm::ivec2>(); }
	};
}
