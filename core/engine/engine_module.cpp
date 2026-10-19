#include <snk/engine/engine_module.h>
#include <snk/render/rnd_render_service_init.h>
#include <snk/gui/gui_gui_service_init.h>
#include <snk/input/inp_input_service_init.h>
#include <snk/input/inp_input_system.h>
#include <snk/window/wnd_window_system.h>
#include <snk/scene/scn_scene_service_init.h>
#include <snk/scene/scn_ecs_assembler.h>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <snk/application/app_frame_stage.h>
#include <snk/application/app_loop_service.h>
#include <snk/common/engine_assert.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/game/gs_engine_frame_stages.h>
#include <snk/gui/gui_system.h>
#include <snk/input/inp_input_consumable_listener.h>
#include <snk/core_input/inp_keyboard_event.hpp>

namespace {
	class content_scale_forwarder final : public inp::input_event_consumable_listener_interface
	{
	public:
		explicit content_scale_forwarder(::gui::gui_system& gui) : m_gui(gui) {}

		bool on_handle_event(wnd::handle, const inp::input_event& evt) override
		{
			if (const auto* scale = evt.get_payload<core::inp::content_scale_changed>()) {
				m_gui.on_content_scale_changed(scale->scale.x);
			}
			return false;
		}

	private:
		::gui::gui_system& m_gui;
	};
}

void engine::engine_module::register_services(ds::app_data_storage& data)
{
	input::input_reg(data);// engine
	render::render_init(data);// engine
	gui::gui_init(data);//engine
	scn::scene_init(data);//engine
	auto& stages = data.require<app::frame_stage_registry>();
	gs::register_scene_frame_stages(stages);
	gs::register_platform_frame_stages(stages);
	if (data.require<app::deterministic_time>().enabled()) {
		// The editor/game builds its worlds synchronously once the level desc is loaded, which is also
		// when the animation controllers start counting.
		auto* loop_impl = dynamic_cast<app::loop_service*>(&data.require<app::app_loop_service_interface>());
		ASSERT_MSG(loop_impl, "engine_module: the frame loop is not app::loop_service");
		if (loop_impl) {
			loop_impl->set_ready_probe([](ds::app_data_storage& storage) {
				return storage.require<scn::context_manager>().get_level().get_world_count() > 0;
			});
		}
	}
	m_scene_types.emplace(scn::register_scene_types(
		data.require<desc::desc_system>(),
		data.require<scn::ecs_assembler>(),
		data.require<rnd::skinning_manager>()));
}

void engine::engine_module::initialize_services(ds::app_data_storage& data)
{
	auto input_service = data.require_shared<inp::input_system>();
	auto& win_service = data.require<wnd::window_system>();
	if (input_service) {
		win_service.add_event_listener(input_service);
		// HACK! first init window size in input system. 
		// Because window created in window system constructor before input system subscribe
		auto window = win_service.get_active_window();
		input_service->on_window_created(window.get());
		input_service->on_window_resize(window.get(), window->get_size().x, window->get_size().y);
		m_content_scale_listener = std::make_shared<content_scale_forwarder>(data.require<::gui::gui_system>());
		input_service->push_consumable_listener(m_content_scale_listener);
	}
	input::input_init(data);
}

void engine::engine_module::shutdown_services(ds::app_data_storage& data)
{
	auto input_service = data.require_shared<inp::input_system>();
	if (input_service) {
		auto& win_service = data.require<wnd::window_system>();
		win_service.remove_event_listener(input_service);
		if (m_content_scale_listener) {
			input_service->pop_consumable_listener(m_content_scale_listener);
		}
	}
	m_content_scale_listener.reset();

	m_scene_types.reset();
	scn::scene_term(data);
	gui::gui_term(data);
	render::render_term(data);
	input::input_term(data);
}