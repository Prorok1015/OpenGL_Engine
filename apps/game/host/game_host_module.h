#pragma once
#include <snk/game_host/game_host.h>
#include <snk/application/app_module_interface.h>
#include <string_view>
#include <vector>

// The shipping host's own module (#591): loads the project's start level on the first frames and ends the run when
// that is asked for or the level cannot be had.
namespace snk::game_host
{
	// What the run came to; run() reads it after the loop, before shutdown removes it.
	struct run_outcome
	{
		int exit_code = 0;
	};

	class game_host_module final : public app::modules::module_interface
	{
	public:
		void register_services(ds::app_data_storage& data) override;
		void initialize_services(ds::app_data_storage& data) override;
		void shutdown_services(ds::app_data_storage& data) override;

		std::string_view name() const override { return "game_host"; }
		std::vector<std::string_view> depends_on() const override { return { "core", "engine" }; }
	};
}
