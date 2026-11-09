#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include <snk/ecs/ecs_bus.hpp>
#include <snk/ecs/ecs_bus_context.hpp>
#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// A game module's bus subscriptions (#603 PR 3, WORLD_BUS.md 3.4) through gs::module_context::subscribe: kept by its
// layer, so they survive a level (data) reload and go with the module on unload and on a code reload.

namespace {

struct module_event
{
	int value = 0;
};

std::vector<int> g_heard_by_first;
std::vector<int> g_heard_by_second;
// Set by the test that unloads from inside a delivery.
app::modules::module_loader* g_loader = nullptr;
ds::app_data_storage* g_root = nullptr;
std::string g_refusal;

template <int N>
class subscribing_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return "subscribing_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		layer.events().add<module_event>();
		data.require<gs::module_context>().subscribe<module_event>({ .set = "level" }, [](ecs::bus_context& ctx, const ecs::envelope<module_event>& e) {
			BOOST_TEST(ctx.world() == "w0");
			(N == 0 ? g_heard_by_first : g_heard_by_second).push_back(e.payload.value);
			if (g_loader) {
				try {
					g_loader->unload("subscribing_game", *g_root);
				} catch (const std::logic_error& error) {
					g_refusal = error.what();
				}
			}
		});
	}
};

constexpr const char* LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[{"__type":"world_desc","name":"w0","systems":[]}]})";

struct bus_module_fixture : test_util::module_env
{
	bus_module_fixture()
	{
		root.attach(test_util::borrowed(tasks));
		g_heard_by_first.clear();
		g_heard_by_second.clear();
		g_loader = nullptr;
		g_root = nullptr;
		g_refusal.clear();
	}

	void load_level()
	{
		BOOST_REQUIRE(contexts().load_level(put("lv.desc", LEVEL)));
		pump();
	}

	void send(int value)
	{
		contexts().emit("level", "w0", module_event{ value });
		contexts().update(std::chrono::duration<float>(0.016f));
	}
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(GsBusSubscriptionTests, bus_module_fixture)

BOOST_AUTO_TEST_CASE(AModulesSubscriptionIsKeptByItsLayerAndHearsItsWorlds)
{
	loader.load(std::make_unique<subscribing_game_module<0>>(), root);
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_TEST(layer->subscriptions().size() == 1u);
	BOOST_TEST(layer->kept_count() == 1u);

	load_level();
	send(1);
	BOOST_TEST((g_heard_by_first == std::vector<int>{ 1 }));
}

BOOST_AUTO_TEST_CASE(ALevelReloadKeepsTheSubscription)
{
	loader.load(std::make_unique<subscribing_game_module<0>>(), root);
	load_level();
	send(1);
	load_level();
	send(2);
	BOOST_TEST((g_heard_by_first == std::vector<int>{ 1, 2 }));
}

BOOST_AUTO_TEST_CASE(UnloadingTheModuleRemovesItsSubscriptions)
{
	loader.load(std::make_unique<subscribing_game_module<0>>(), root);
	load_level();
	send(1);
	loader.unload("subscribing_game", root);

	auto* fresh = contexts().game_layer();
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(fresh->subscriptions().size() == 0u);
	BOOST_TEST(!fresh->events().contains<module_event>());
	BOOST_TEST(contexts().level_set().size() == 0u); // its worlds went with its layer
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST((g_heard_by_first == std::vector<int>{ 1 }));
}

BOOST_AUTO_TEST_CASE(ACodeReloadLeavesOnlyTheNewModulesSubscriptions)
{
	loader.load(std::make_unique<subscribing_game_module<0>>(), root);
	load_level();
	send(1);

	// What a code reload does to the layer: the old module goes with its layer, the new one fills a fresh one.
	loader.unload("subscribing_game", root);
	loader.load(std::make_unique<subscribing_game_module<1>>(), root);
	BOOST_TEST(contexts().game_layer()->subscriptions().size() == 1u);
	load_level();
	send(2);

	BOOST_TEST((g_heard_by_first == std::vector<int>{ 1 }));
	BOOST_TEST((g_heard_by_second == std::vector<int>{ 2 }));
}

BOOST_AUTO_TEST_CASE(AnUnloadFromInsideADeliveryIsRefused)
{
	loader.load(std::make_unique<subscribing_game_module<0>>(), root);
	load_level();
	g_loader = &loader;
	g_root = &root;
	send(1);
	g_loader = nullptr;

	BOOST_TEST(g_refusal.find("during the scene's update") != std::string::npos);
	BOOST_TEST((loader.state("subscribing_game") == app::modules::module_state::initialized));
	send(2); // the module, its layer and its subscription are all still there
	BOOST_TEST((g_heard_by_first == std::vector<int>{ 1, 2 }));
}

BOOST_AUTO_TEST_SUITE_END()
