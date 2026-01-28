#include "inp_input_service_init.h"
#include "inp_input_system.h"
#include "inp_ecs_input_manager.h"
#include "ecs_system.h"

void engine::input::input_reg(ds::app_data_storage& data)
{
	data.construct<inp::input_system>();
	data.construct<inp::ecs_input_manager>();
}

void engine::input::input_init(ds::app_data_storage& data)
{
	auto& input_service = data.require<inp::input_system>();
	auto ecs_input = data.require_shared<inp::ecs_input_manager>();

	input_service.push_input_layer(input_service.get_focused_window(), inp::input_layer{ ecs_input });

	auto& sfactory = data.require<ecs::system_factory>();
	sfactory.register_automatic_system<&inp::ecs_input_manager::update_input_state_system>("inp::update_input_state", ecs_input);
}

void engine::input::input_term(ds::app_data_storage& data)
{
	data.destruct<inp::input_system>();
}
