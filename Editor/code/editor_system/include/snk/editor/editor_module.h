#pragma once
#include <snk/application/app_module_interface.h>
#include <string_view>
#include <vector>

namespace editor
{
	// Settings the editor needs set before its modules register (the gui module, which registers first, reads them):
	// gui/layout_file defaults to settings/layout_settings.ini in the editor's user folder (editor/user_data_path, else the
	// per-user default), so the window layout is never written into the folder the process was started from (#713). A value
	// the config or --set gave stays. Call after cfg::initialize_configs.
	void apply_default_settings();

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
