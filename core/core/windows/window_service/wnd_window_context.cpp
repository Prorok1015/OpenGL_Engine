#include <GLFW/glfw3.h>
#include "wnd_window_context.h"
 
wnd::context::context(wnd::context::header title_)
	: title(std::move(title_))
{
	internal_id = glfwCreateWindow(title.size.x, title.size.y, title.title.data(), NULL, NULL);
	ASSERT_MSG(internal_id, "Window didn't create!");
}

wnd::context::~context() {
	glfwDestroyWindow(internal_id);
}

void wnd::context::swap_buffers()
{
 	glfwSwapBuffers(internal_id);
	set_next_frame();
}
 
