#include "window_system.h"
#include "window.h"
#include "../common/ds_store.hpp"

using namespace application;

static void window_size_callback(GLFWwindow* window, int width, int height) {
    auto wndCreator = ds::DataStorage::instance().require<WindowSystem>();
    auto wnd = wndCreator->find_window(window);
    if (auto swnd = wnd.lock()) {
        swnd->on_resize_window(width, height);
    }
}

static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos) {
    auto wndCreator = ds::DataStorage::instance().require<WindowSystem>();
    auto wnd = wndCreator->find_window(window);
    if (auto swnd = wnd.lock()) {
        swnd->on_mouse_move(xpos, ypos);
    }
}

static void mouse_button_callback(GLFWwindow* window, int button, int action, int mode) {
    auto wndCreator = ds::DataStorage::instance().require<WindowSystem>();
    auto wnd = wndCreator->find_window(window);
    if (auto swnd = wnd.lock()) {
        swnd->on_mouse_button_action(button, action, mode);
    }
}

static void key_callback(GLFWwindow* window, int keycode, int scancode, int action, int mode) {
    auto wndCreator = ds::DataStorage::instance().require<WindowSystem>();
    auto wnd = wndCreator->find_window(window);
    if (auto swnd = wnd.lock()) {
        swnd->on_keyboard_action(keycode, scancode, action, mode);
    }
}

application::WindowSystem::WindowSystem()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, GL_TRUE);
}

application::WindowSystem::~WindowSystem()
{
    glfwTerminate();
}

std::shared_ptr<Window> application::WindowSystem::make_window(std::string_view title, int width, int height)
{
    auto shared_window = std::make_shared<Window>(title, width, height);
    auto window = shared_window->window;
    windowList[window] = shared_window;

    glfwSetWindowSizeCallback(window, window_size_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetCursorPosCallback(window, cursor_position_callback);

    return shared_window;
}

std::weak_ptr<Window> application::WindowSystem::find_window(GLFWwindow* win)
{
    return windowList[win];
}