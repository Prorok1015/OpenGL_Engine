#include "wnd_null_window_system.h"
#include "engine_log.h"

void wnd::null_window::set_title(std::string_view title)
{
	if (title.empty()) {
		egLOG("window/title", "Trying to set empty title");
		return;
	}
	m_title = title;
}

wnd::null_window_system::null_window_system()
	: m_window(std::make_shared<null_window>())
{
}

wnd::null_window_system::~null_window_system() = default;
