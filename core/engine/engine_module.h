#pragma once
#include <snk/application/app_module_interface.h>
#include "scn_registration_scope.h"
#include <memory>
#include <optional>
#include <snk/common/snk_api.h>

namespace inp { class input_event_consumable_listener_interface; }

namespace engine
{
	class SNK_API engine_module : public app::modules::module_interface
	{
	public:
		virtual void register_services(ds::app_data_storage& data) override;
		virtual void initialize_services(ds::app_data_storage& data) override;
		virtual void shutdown_services(ds::app_data_storage& data) override;

		virtual std::string_view name() const override { return "engine"; }
		virtual std::vector<std::string_view> depends_on() const override { return { "core" }; }

	private:
		// Spawners hold &ecs_assembler: reset in shutdown_services before the scene services go.
		std::optional<scn::registration_scope> m_scene_types;
		// Forwards content_scale_changed to gui_system (font rebuild); never consumes the event.
		std::shared_ptr<inp::input_event_consumable_listener_interface> m_content_scale_listener;
	};
}