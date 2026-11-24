#pragma once
#include <snk/application/app_module_interface.h>
#include <filesystem>
#include <functional>
#include <string_view>
#include <vector>

namespace core
{
	struct library_copy;
}

namespace editor
{
	// Settings the editor needs set before its modules register (the gui module, which registers first, reads them):
	// gui/layout_file defaults to settings/layout_settings.ini in the editor's user folder (editor/user_data_path, else the
	// per-user default), so the window layout is never written into the folder the process was started from (#713). A value
	// the config or --set gave stays. Call after cfg::initialize_configs.
	void apply_default_settings();

	// core::project_modules_options::trust_library of the editor (#921): the person's trust in the project, kept by
	// the editor (edt::project_trust_service in `data`, made by the editor's init); `trust_flag` (--trust-project)
	// trusts everything for this run. Without the editor in `data`, only the flag counts. `data` outlives the gate.
	std::function<bool(std::string_view name, const core::library_copy& copy)> project_trust_gate(
		ds::app_data_storage& data, bool trust_flag);

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
