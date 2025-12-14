#include "wnd_window_system.h"
#include "wnd_window.h"
#include "rnd_gl_render_context.h"
#include "gui_gl_backend.h"
#include "wnd_input_keycode_convert.hpp"
#include "wnd_keyboard_event.hpp"
#include <filesystem>

namespace {
    void window_focus_callback(GLFWwindow* window, int focused)
    {
        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window({ window })) {
            for (auto listener : wndCreator.get_event_listeners()) {
                if (focused)
                    listener->on_window_focus_gained(win.get());
                else 
                    listener->on_window_focus_lost(win.get());
            }
        }
    }

    void window_size_callback(GLFWwindow* window, int width, int height) {
		auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window({ window })) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_window_resize(win.get(), width, height);
            }
        }

        if (auto wnd = wndCreator.find_window({ window })) {
            wnd->on_resize_window(width, height);
        }
    }

    void device_scroll_callback(GLFWwindow* window, double xoffset, double yoffset) {
        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window({ window })) {

			wnd::input_event evt;
			evt.construct_payload<wnd::scroll_move_event>(glm::vec2{ (float)xoffset, (float)yoffset });

            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
                listener->on_mouse_scrolled(win.get(), xoffset, yoffset);
            }
        }
    }

    void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);

		wnd::input_event evt;
        evt.construct_payload<wnd::cursor_move_event>(glm::vec2{ xpos, ypos });

        if (auto win = wndCreator.find_window({ window })) {
            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
                listener->on_mouse_moved(win.get(), xpos, ypos);
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

		wnd::input_event evt;
		evt.construct_payload<wnd::mouse_click_event>(key_optional.value(), wnd::convert::to_action(action));

        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window({ window })) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_mouse_button_input(win.get(), key_optional.value(), wnd::convert::to_action(action), mode);
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

		wnd::input_event evt;
		evt.construct_payload<wnd::keyboard_event>(key_optional.value(), wnd::convert::to_action(action));

        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);
        if (auto win = wndCreator.find_window({ window })) {
            for (auto& listener : wndCreator.get_event_listeners()) {
				listener->on_input_event(win.get(), evt);
                listener->on_key_input(win.get(), key_optional.value(), scancode, wnd::convert::to_action(action), mode);
            }
        }
   }

    void window_refresh_callback(GLFWwindow* window) {
        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);

        if (auto win = wndCreator.find_window({ window })) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_window_refresh(win.get());
            }
        }
    }

    void window_move_callback(GLFWwindow* window, int xpos, int ypos) {
        auto& wndCreator = *(wnd::window_system*)glfwGetWindowUserPointer(window);

        if (auto win = wndCreator.find_window({ window })) {
            for (auto& listener : wndCreator.get_event_listeners()) {
                listener->on_window_moved(win.get(), xpos, ypos);
            }
        }
    }
}

wnd::window_system::window_system()
{
    glfwInit();
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // TODO: move to gl context
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GL_TRUE);

    active_window = make_window()->get_id();   
    context = std::make_unique<rnd::driver::gl::render_context>((GLADloadproc)glfwGetProcAddress);
    imgui_backend = std::make_unique<gui::gl::gl_imgui_backend>();
    imgui_backend->init(active_window.internal_id);
    auto is_exist = std::filesystem::exists(imgui_backend->get_settings_filename());
    imgui_backend->set_initial_layout_by_default(!is_exist);
}

wnd::window_system::~window_system()
{
    imgui_backend->shutdown();
    glfwTerminate();
}

std::shared_ptr<wnd::window> wnd::window_system::make_window()
{
    auto shared_window = std::make_shared<window>(wnd::context{ title });

    window::short_id wid = shared_window->get_id();
    windows_list[wid] = shared_window;

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

std::shared_ptr<wnd::window> wnd::window_system::get_active_window()
{
    return find_window(active_window);
}

std::shared_ptr<wnd::window> wnd::window_system::find_window(window::short_id win)
{
    return windows_list[win];
}

void wnd::window_system::pool_events() const
{
    glfwPollEvents();
}


void wnd::window_system::init_windows_frame_time() const
{
    for (const auto& [_, win] : windows_list) {
        win->init_frame();
    }
}

void wnd::window_system::process_windows() const
{
    for (const auto& [_, win] : windows_list) {
        win->update_frame();
        win->on_update();
    }
}

bool wnd::window_system::is_stop_running()
{
    for (const auto& [_, win] : windows_list)
    { 
        if (!win->is_shutdown()) {
            return false;
        } 
    }
    return true;
}
