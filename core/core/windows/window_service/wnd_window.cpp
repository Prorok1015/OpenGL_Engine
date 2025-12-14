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

void wnd::window::set_logo(const ds::fixed_vector<wnd::window_image, 2>& images_)
{
    if (images_.empty()) {
        egLOG("window/logo", "Logo should be rgba image");
        return;
    }

    GLFWimage images[2]{};
	for (const auto& img : images_) {
        switch (img.type)
        {
            case wnd::window_image::TYPE::LOGO:
            {
                if (img.width <= 0 || img.height <= 0 || !img.pixels) {
                    egLOG("window/logo", "Logo image is invalid");
                    continue;
                }
                if (img.width % 4 != 0) {
                    egLOG("window/logo", "Logo image width should be multiple of 4");
                    continue;
                }
                images[0].pixels = img.pixels;
                images[0].width = img.width;
                images[0].height = img.height;
            }
			break;

            case wnd::window_image::TYPE::ICON:
            {
                if (img.width <= 0 || img.height <= 0 || !img.pixels) {
                    egLOG("window/logo", "Logo small image is invalid");
                    continue;
                }
                if (img.width % 4 != 0) {
                    egLOG("window/logo", "Logo small image width should be multiple of 4");
                    continue;
                }
                images[1].pixels = img.pixels;
                images[1].width = img.width;
                images[1].height = img.height;
            }
            break;
        }
    }

    glfwSetWindowIcon(ctx.get_id(), images_.size(), images);
}

glm::ivec2 wnd::window::get_backbuffer_size() const
{
    glm::ivec2 size{ 0 };
    glfwGetFramebufferSize(ctx.get_id(), &size.x, &size.y);
    return size;
}