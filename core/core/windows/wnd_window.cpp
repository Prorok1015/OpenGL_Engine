#include "wnd_window.h"
#include <GLFW/glfw3.h>
#include <timer.hpp>
#include <engine_log.h>

wnd::window::window(wnd::context ctx_)
    : ctx(std::move(ctx_)), size(ctx.get_header().size)
{
    /* Make the window's context current */
    glfwMakeContextCurrent(ctx.get_id());
    init_frame();
}


wnd::window::~window()
{
}

void wnd::window::set_vsync(bool val)
{
    /* Make the window's context current */
    glfwMakeContextCurrent(ctx.get_id());
    glfwSwapInterval(val ? 1 : 0);
}

void wnd::window::on_update()
{
    ctx.swap_buffers();
}

bool wnd::window::is_shutdown() const
{
    return glfwWindowShouldClose(ctx.get_id());
}

void wnd::window::shutdown(bool close)
{
    glfwSetWindowShouldClose(ctx.get_id(), close);
}

void wnd::window::set_cursor_mode(CURSOR_MODE mode) {
	int glfw_mode = 0;
    switch (mode)
    {
    case CURSOR_MODE::DISABLE:
        glfw_mode = GLFW_CURSOR_DISABLED;
        break;
    case CURSOR_MODE::NORMAL:
        glfw_mode = GLFW_CURSOR_NORMAL;
        break;
    case CURSOR_MODE::HIDDEN:
        glfw_mode = GLFW_CURSOR_HIDDEN;
        break;
    }

    glfwSetInputMode(ctx.get_id(), GLFW_CURSOR, glfw_mode);
}

void wnd::window::init_frame()
{
    lastTime = Timer::now();
    delta = 0;
}

void wnd::window::update_frame()
{
    double currentTime = Timer::now();
    delta = currentTime - lastTime;
    lastTime = currentTime;
}

void wnd::window::on_resize_window(int width, int height)
{
    size = { width, height };
    eventResizeWindow(*this, width, height);
}

void wnd::window::set_title(std::string_view title)
{
    if (title.empty()) {
        egLOG("window/title", "Trying to set empty title");
        return;
    }

    glfwSetWindowTitle(ctx.get_id(), title.data());
}

void wnd::window::set_logo(res::ImageRef logo, res::ImageRef logo_small)
{
    if (logo->channels() != 4) {
        egLOG("window/logo", "Logo should be rgba image");
        return;
    }

    GLFWimage images[2];
    images[0].pixels = logo->data();
    images[0].width = logo->size().x;
    images[0].height = logo->size().y;

    if (logo_small) {
        images[1].pixels = logo_small->data();
        images[1].width = logo_small->size().x;
        images[1].height = logo_small->size().y;
        glfwSetWindowIcon(ctx.get_id(), 2, images);
        return;
    }

    glfwSetWindowIcon(ctx.get_id(), 1, images);
}

glm::ivec2 wnd::window::get_backbuffer_size() const
{
    glm::ivec2 size{ 0 };
    glfwGetFramebufferSize(ctx.get_id(), &size.x, &size.y);
    return size;
}