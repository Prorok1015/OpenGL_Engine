#pragma once
#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include "wnd_window_context.h"
#include "ds_fixed_vector.hpp"

struct GLFWwindow;

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

	class window
	{
	public:
		struct short_id
		{
			GLFWwindow* internal_id = nullptr;

			auto operator<=>(const short_id&) const = default;
			std::size_t get_id() const { return std::size_t(internal_id); }
			operator GLFWwindow* () const { return internal_id; }
			struct hasher {
				std::size_t operator() (const short_id& id) const { return id.get_id(); }
			};
		};

	public:
		window(context ctx_);
		~window();
		float aspect_ratio() const { if (is_minimize_mode()) return 0.f; return (float)size.x / (float)size.y; }
		bool is_shutdown() const;
		void on_update();
		void shutdown(bool close = true);
		void set_cursor_mode(CURSOR_MODE mode);
		void update_frame();
		void init_frame();

		void on_resize_window(int width, int height);

		glm::ivec2 get_backbuffer_size() const;

		void set_logo(const ds::fixed_vector<window_image, 2>& images);
		void set_title(std::string_view title);
		void set_vsync(bool v);

		bool is_minimize_mode() const {
			return size == glm::zero<glm::ivec2>();
		}

		float get_delta() const { return (float)delta; }
		const glm::ivec2& get_size() const { return size; }
		short_id get_id() const { return short_id{ ctx.get_id() }; }

	public:
		Event<void(window& window)> eventRefreshWindow;//TODO: remove
		Event<void(window& window, int width, int height)> eventResizeWindow;//TODO: remove

	private: 
		context ctx;
		glm::ivec2 size{ 0 };
		double delta = 0.f;
		double lastTime = 0.f;
	};
}
