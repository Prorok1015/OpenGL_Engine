#include "inp_input_manager.h"
#include "common.h"


bool inp::input_manager::on_handle_event(const input_event& evt)
{
    return false;
}

void inp::input_manager::notify_listeners(float dt)
{
    if (!get_is_enabled()) {
        return;
    }

    auto& commands = get_active_commands();
    std::for_each(commands.begin(), commands.end(), [dt](auto cmd) { cmd->update(dt); });
}

void inp::input_manager::unregistrate(std::shared_ptr<inp::InputActionBase> command)
{
    auto& commands = get_active_commands();
    commands.erase(std::remove(commands.begin(), commands.end(), command));
};
