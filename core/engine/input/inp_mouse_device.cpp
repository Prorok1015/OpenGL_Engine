#include "inp_mouse_device.h"
#include "engine_log.h"

void inp::MouseDevice::on_mouse_move(double xpos, double ypos)
{
	prev = cur;
	cur = { xpos, ypos };
	//egLOG("", "Mouse pos x:{}, y:{}; prev x:{}, y:{}", position.x, position.y, prev_position.x, prev_position.y);
}

void inp::MouseDevice::on_mouse_scroll(double xpos, double ypos)
{
	scroll_direction = { xpos, ypos };
}

void inp::MouseDevice::on_mouse_button_action(MOUSE_BUTTONS button, KEY_ACTION action, int mode)
{
    if (action == KEY_ACTION::NONE) {
        return;
    }

    keys_[(int)button] = action;
}

inp::Key inp::MouseDevice::get_key(MOUSE_BUTTONS key) const
{
	KEY_ACTION action = keys_[std::size_t(key)];
	return { action };
}
