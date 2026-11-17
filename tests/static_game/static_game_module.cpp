#include <snk/application/app_module_entry.h>
#include <snk/common/ds/ds_store.hpp>
#include <snk/common/logger/engine_log.h>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/scene/scn_scene_layer.h>
#include <string>
#include <string_view>
#include <vector>

// A game module compiled into its host (#592, snk_add_game_host in CMakeLists.txt): the CI run of
// test_static_game_host checks that its system runs in the project's start level.
namespace {

struct static_game_state {
	bool ran = false;
};

void static_game_tick(static_game_state& state)
{
	if (!state.ran) {
		state.ran = true;
		egLOG_INFO("static_game", "static game system ran");
	}
}

class static_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return SNK_GAME_MODULE_NAME; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		// tests/static_game/project/res/levels/main.desc lists it.
		layer.systems().register_system<&static_game_tick>(std::string{ "static_game::tick" });
	}
};

}

SNK_DECLARE_MODULE(static_game_module)
