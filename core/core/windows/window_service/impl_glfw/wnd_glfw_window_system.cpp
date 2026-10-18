#include "wnd_glfw_window_system.h"
#include "rnd_gl_render_context.h"
#include "wnd_input_keycode_convert.hpp"
#include "inp_keyboard_event.hpp"
#include "inp_input_event.hpp"
#include "engine_log.h"
#include <filesystem>

namespace {
    void window_focus_callback(GLFWwindow* window, int focused)
    {
        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window(window)) {
            for (auto listener : wndCreator.get_event_listeners()) {
                if (focused)
                    listener->on_window_focus_gained(win.get());
                else
                    listener->on_window_focus_lost(win.get());
            }
        }
    }

    void window_size_callback(GLFWwindow* window, int width, int height) {
		auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window(window)) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_window_resize(win.get(), width, height);
            }
        }
    }

    void device_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window(window)) {

			inp::input_event evt;
			evt.construct_payload<core::inp::scroll_move_event>(glm::vec2{ (float)xoffset, (float)yoffset });

            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
            }
        }
    }

    void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);

		inp::input_event evt;
        evt.construct_payload<core::inp::cursor_move_event>(glm::vec2{ xpos, ypos });

        if (auto win = wndCreator.find_window(window)) {
            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
            }
        }
    }

    void mouse_button_callback(GLFWwindow* window, int button, int action, int mode) {
        if (button == -1 || action == GLFW_REPEAT) {
            return;
        }

        auto key_optional = wnd::convert::to_mouse_keycode(button);
        if (!key_optional.has_value()) {
            return;
        }

		inp::input_event evt;
		evt.construct_payload<core::inp::mouse_click_event>(key_optional.value(), wnd::convert::to_action(action));

        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window(window)) {
            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
            }
        }
    }

    void key_callback(GLFWwindow* window, int keycode, int scancode, int action, int mode) {
        if (keycode == -1 || action == GLFW_REPEAT) {
            return;
        }

        auto key_optional = wnd::convert::to_keyboard_keycode(keycode);
        if (!key_optional.has_value()) {
            return;
        }

		inp::input_event evt;
		evt.construct_payload<core::inp::keyboard_event>(key_optional.value(), wnd::convert::to_action(action));

        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window(window)) {
            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
            }
        }
   }

    void window_refresh_callback(GLFWwindow* window) {
        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);

        if (auto win = wndCreator.find_window(window)) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_window_refresh(win.get());
            }
        }
    }

    void window_move_callback(GLFWwindow* window, int xpos, int ypos) {
        auto& wndCreator = *(wnd::glfw_window_system*)glfwGetWindowUserPointer(window);

        if (auto win = wndCreator.find_window(window)) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_window_moved(win.get(), xpos, ypos);
            }
        }
    }
}

wnd::glfw_window_system::glfw_window_system()
{
    glfwSetErrorCallback([](int code, const char* description) {
        egLOG_ERROR("window", "GLFW error {}: {}", code, description);
    });
    glfwInit();
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // TODO: move to gl context
    // 4.5 is what the GL backend actually requires (DSA); asking for 4.6 fails on 4.5-only drivers.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);

    m_active_window = make_window()->get_glfw_handle();
    m_context = std::make_unique<rnd::driver::gl::render_context>((GLADloadproc)glfwGetProcAddress);
    // The imgui backend is now created/initialised/shut down by the GUI layer (gui_init/gui_term),
    // so window no longer depends on gui (EPIC-49 US-49-9). It exposes the native handle + DPI
    // scale below for the GUI layer to set its backend up.
}

wnd::glfw_window_system::~glfw_window_system()
{
    // Members are destroyed after the body; wnd::context::~context calls glfwDestroyWindow,
    // which would run after glfwTerminate and raise GLFW_NOT_INITIALIZED (#182).
    m_context.reset();
    m_windows_list.clear();
    glfwTerminate();
}

float wnd::glfw_window_system::get_active_content_scale() const
{
    if (auto win = find_window(m_active_window)) {
        return win->get_content_scale();
    }
    return 1.f;
}

void wnd::glfw_window_system::init()
{

}

std::shared_ptr<wnd::glfw_window> wnd::glfw_window_system::make_window()
{
    auto shared_window = std::make_shared<glfw_window>(wnd::context{ m_title });

    GLFWwindow* wid = shared_window->get_glfw_handle();
    m_windows_list[wid] = shared_window;

	glfwSetWindowUserPointer(wid, this);

    glfwSetKeyCallback(wid, key_callback);
    glfwSetMouseButtonCallback(wid, mouse_button_callback);
    glfwSetScrollCallback(wid, device_scroll_callback);
    glfwSetCursorPosCallback(wid, cursor_position_callback);

    glfwSetWindowRefreshCallback(wid, window_refresh_callback);
    glfwSetWindowPosCallback(wid, window_move_callback);
    glfwSetWindowSizeCallback(wid, window_size_callback);
    glfwSetWindowFocusCallback(wid, window_focus_callback);

    for (auto& listener : get_event_listeners()) {
        listener->on_window_created(shared_window.get());
		listener->on_window_resize(shared_window.get(), shared_window->get_size().x, shared_window->get_size().y);
    }

    return shared_window;
}

std::shared_ptr<wnd::window> wnd::glfw_window_system::get_active_window()
{
    return find_window(m_active_window);
}

std::shared_ptr<wnd::window> wnd::glfw_window_system::find_window(GLFWwindow* handle) const
{
    auto it = m_windows_list.find(handle);
    return it != m_windows_list.end() ? it->second : nullptr;
}

void wnd::glfw_window_system::pool_events() const
{
    glfwPollEvents();
}


void wnd::glfw_window_system::init_windows_frame_time() const
{
    for (const auto& [_, win] : m_windows_list) {
        win->init_frame();
    }
}

void wnd::glfw_window_system::process_windows() const
{
    for (const auto& [_, win] : m_windows_list) {
        win->update_frame();
        win->on_update();
    }
}

bool wnd::glfw_window_system::is_stop_running()
{
    for (const auto& [_, win] : m_windows_list)
    {
        if (!win->is_shutdown()) {
            return false;
        }
    }
    return true;
}
