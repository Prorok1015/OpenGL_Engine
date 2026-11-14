#include <snk/application/app_module_entry.h>
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_describe_ops.h>
#include <snk/desc/desc_schema_desc.h>
#include <snk/desc/desc_system.h>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/resource/res_system.h>
#include <snk/resource/res_tag.h>
#include <snk/scene/scn_scene_layer.h>
#include <memory>
#include <string>
#include <string_view>

// The project's game code. Register components, systems, descs and spawners into `layer` (see gs::game_module).
//
// The game's config is the example of a data document of the game: game_config below is described once, registered as a
// document, and res/config/game.desc holds one. Double click the file in the Asset Browser to edit it in the inspector and
// save with Ctrl+S: a system that reads it as game_config_state (read_game_config) sees the new value without rebuilding
// this library. See README.md in this folder's parent for the steps.

namespace {

class game_config final : public desc::schema_desc<game_config>
{
public:
	static constexpr std::string_view __type = "game.config";
	using base_type = desc::desc_base; // the resource system finds the desc adapter through it

	float player_speed = 4.0f;
	int goal = 10;

	BOOST_DESCRIBE_CLASS(game_config, (), (player_speed, goal), (), ())
};

// What the game's systems read: the value the config had the last time it was loaded.
struct game_config_state {
	float player_speed = 4.0f;
	int goal = 10;
};

res::resource_system* g_resources = nullptr;

// Ask for the handle every frame: while the file is unchanged it is a hit of the cache. A save in the editor makes the next
// request load the file again; until it is ready the system keeps the previous value.
void read_game_config(game_config_state& state)
{
	if (!g_resources) return;
	const std::shared_ptr<game_config> config = g_resources->require<game_config>(res::tag{ std::string{ "res://config/game.desc" } }).get();
	if (!config) return;
	state.player_speed = config->player_speed;
	state.goal = config->goal;
}

class project_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return SNK_GAME_MODULE_NAME; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		layer.descs().register_document<game_config>("game.config");
		g_resources = &layer.resources();
		// Add "game::read_config" to the "systems" of a world in res/levels/main.desc to run it there.
		layer.systems().register_system<&read_game_config>(std::string{ "game::read_config" });
	}

	void shutdown_content(scn::scene_layer&, ds::app_data_storage&) override { g_resources = nullptr; }
};

}

SNK_DECLARE_MODULE(project_game_module)
