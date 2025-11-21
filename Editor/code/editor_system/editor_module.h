#pragma once
#include <eng_module_interface.h>

namespace editor
{
	using MODULE_PRIORITY = modules::MODULE_PRIORITY;
	class editor_module : public modules::module_interface
	{
	public:
		virtual void register_services(ds::app_data_storage& data) override;
		virtual void initialize_services(ds::app_data_storage& data) override;
		virtual void shutdown_services(ds::app_data_storage& data) override;
		virtual MODULE_PRIORITY get_priority() const override {
			return MODULE_PRIORITY::EDITOR;
		}
	};
}
