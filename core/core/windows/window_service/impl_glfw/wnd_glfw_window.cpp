#include "wnd_glfw_window.h"
#include <GLFW/glfw3.h>
#include <timer.hpp>
#include <engine_log.h>

wnd::glfw_window::glfw_window(wnd::context ctx_)
    : m_ctx(std::move(ctx_)), m_size(m_ctx.get_header().size)
{
    if (m_ctx.get_header().api == graphics_api::opengl) {
        make_current();
    }
    init_frame();
}


wnd::glfw_window::~glfw_window()
{
    for (GLFWcursor* cursor : m_cursors) {
        if (cursor) {
            glfwDestroyCursor(cursor);
        }
    }
}

wnd::gl_surface* wnd::glfw_window::gl_surface_ptr() const
{
    if (m_ctx.get_header().api != graphics_api::opengl) {
        return nullptr;
    }
    return const_cast<glfw_window*>(this);
}

void wnd::glfw_window::make_current()
{
    glfwMakeContextCurrent(m_ctx.get_id());
}

void wnd::glfw_window::swap_buffers()
{
    m_ctx.swap_buffers();
}

void wnd::glfw_window::set_swap_interval(int interval)
{
    glfwSwapInterval(interval);
}

void* wnd::glfw_window::get_proc_address(const char* name) const
{
    return reinterpret_cast<void*>(glfwGetProcAddress(name));
}

bool wnd::glfw_window::is_shutdown() const
{
    return glfwWindowShouldClose(m_ctx.get_id());
}

void wnd::glfw_window::shutdown(bool close)
{
    glfwSetWindowShouldClose(m_ctx.get_id(), close);
}

void wnd::glfw_window::set_cursor_mode(CURSOR_MODE mode) {
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

    glfwSetInputMode(m_ctx.get_id(), GLFW_CURSOR, glfw_mode);
}

void wnd::glfw_window::set_cursor_shape(cursor_shape shape)
{
    const auto index = static_cast<size_t>(shape);
    if (index >= m_cursors.size()) {
        return;
    }

    if (!m_cursors[index]) {
        int glfw_shape = GLFW_ARROW_CURSOR;
        switch (shape)
        {
        case cursor_shape::arrow:       glfw_shape = GLFW_ARROW_CURSOR; break;
        case cursor_shape::text_input:  glfw_shape = GLFW_IBEAM_CURSOR; break;
        case cursor_shape::resize_all:  glfw_shape = GLFW_RESIZE_ALL_CURSOR; break;
        case cursor_shape::resize_ns:   glfw_shape = GLFW_RESIZE_NS_CURSOR; break;
        case cursor_shape::resize_ew:   glfw_shape = GLFW_RESIZE_EW_CURSOR; break;
        case cursor_shape::resize_nesw: glfw_shape = GLFW_RESIZE_NESW_CURSOR; break;
        case cursor_shape::resize_nwse: glfw_shape = GLFW_RESIZE_NWSE_CURSOR; break;
        case cursor_shape::hand:        glfw_shape = GLFW_POINTING_HAND_CURSOR; break;
        case cursor_shape::not_allowed: glfw_shape = GLFW_NOT_ALLOWED_CURSOR; break;
        case cursor_shape::count:       break;
        }
        // Null when the platform lacks the shape: glfwSetCursor(nullptr) restores the default arrow.
        m_cursors[index] = glfwCreateStandardCursor(glfw_shape);
    }

    glfwSetCursor(m_ctx.get_id(), m_cursors[index]);
}

std::string wnd::glfw_window::get_clipboard_text() const
{
    const char* text = glfwGetClipboardString(m_ctx.get_id());
    return text ? std::string{ text } : std::string{};
}

void wnd::glfw_window::set_clipboard_text(std::string_view text)
{
    const std::string owned{ text };
    glfwSetClipboardString(m_ctx.get_id(), owned.c_str());
}

void wnd::glfw_window::init_frame()
{
    m_last_time = Timer::now();
    m_delta = 0;
}

void wnd::glfw_window::update_frame()
{
    double currentTime = Timer::now();
    m_delta = currentTime - m_last_time;
    m_last_time = currentTime;
}

void wnd::glfw_window::set_title(std::string_view title)
{
    if (title.empty()) {
        egLOG("window/title", "Trying to set empty title");
        return;
    }

    const std::string owned_title{ title };
    glfwSetWindowTitle(m_ctx.get_id(), owned_title.c_str());
}

void wnd::glfw_window::set_logo(const ds::fixed_vector<wnd::window_image, 2>& images_)
{
    if (images_.empty()) {
        egLOG("window/logo", "Logo should be rgba image");
        return;
    }

    GLFWimage images[2]{};
    int count = 0;
	for (const auto& img : images_) {
        if (count >= 2) {
            break;
        }
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
                images[count++] = GLFWimage{ img.width, img.height, img.pixels };
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
                images[count++] = GLFWimage{ img.width, img.height, img.pixels };
            }
            break;
        }
    }

    if (count > 0) {
        glfwSetWindowIcon(m_ctx.get_id(), count, images);
    }
}

glm::ivec2 wnd::glfw_window::get_backbuffer_size() const
{
    glm::ivec2 size{ 0 };
    glfwGetFramebufferSize(m_ctx.get_id(), &size.x, &size.y);
    return size;
}

float wnd::glfw_window::get_content_scale() const
{
    float xscale = 1.f;
    glfwGetWindowContentScale(m_ctx.get_id(), &xscale, nullptr);
    return xscale;
}
