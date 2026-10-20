#pragma once
#include <snk/application/app_module_interface.h>

namespace editor
{
	class editor_module : public app::modules::module_interface
	{
	public:
		virtual void register_services(ds::app_data_storage& data) override;
		virtual void initialize_services(ds::app_data_storage& data) override;
		virtual void shutdown_services(ds::app_data_storage& data) override;

		virtual std::string_view name() const override { return "editor"; }
		virtual std::vector<std::string_view> depends_on() const override { return { "core", "engine" }; }
	};
}
