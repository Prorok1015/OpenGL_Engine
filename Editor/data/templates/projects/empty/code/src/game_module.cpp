#include <snk/application/app_module_entry.h>
#include <snk/game/gs_game_module.h>
#include <string_view>

// The project's game code. Register components, systems, descs and spawners into `layer` (see gs::game_module).

namespace {

class project_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return SNK_GAME_MODULE_NAME; }

protected:
	void register_content(scn::scene_layer&, ds::app_data_storage&) override {}
};

}

SNK_DECLARE_MODULE(project_game_module)
