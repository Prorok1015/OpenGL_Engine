#include <GLFW/glfw3.h>
#include "wnd_window_context.h"
 
wnd::context::context(wnd::context::header title_)
	: title(std::move(title_))
{
	if (title.api == graphics_api::opengl) {
		// 4.5 is what the GL backend actually requires (DSA); asking for 4.6 fails on 4.5-only drivers.
		glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
		glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
	} else {
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	}
	internal_id = glfwCreateWindow(title.size.x, title.size.y, title.title.data(), NULL, NULL);
	ASSERT_MSG(internal_id, "Window didn't create!");
	if (!internal_id)
		throw std::runtime_error("Window didn't create!");
}

wnd::context::~context() {
	glfwDestroyWindow(internal_id);
}

void wnd::context::swap_buffers()
{
 	glfwSwapBuffers(internal_id);
	set_next_frame();
}
 
