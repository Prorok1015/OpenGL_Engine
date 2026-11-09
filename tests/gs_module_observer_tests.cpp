#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include <snk/ecs/ecs_bus_context.hpp>
#include <snk/ecs/ecs_signal.hpp>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_event_subscriptions.h>
#include <snk/scene/scn_scene_layer.h>
#include <entt/entity/registry.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Component observers of a game module (#600, HOT_RELOAD_PLAY.md 6 step 4) through gs::module_context::observe: kept by
// its layer, heard in its worlds from their first entity, silent while muted (a restore puts back entities that
// already existed), and gone with the module.

namespace {

struct module_marker
{
	int value = 0;
};

std::vector<int> g_constructed;
int g_destroyed = 0;
ecs::connection* g_construct_connection = nullptr;

class observing_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return "observing_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer&, ds::app_data_storage& data) override
	{
		auto& context = data.require<gs::module_context>();
		g_construct_connection = &context.observe<module_marker>({ .set = "level" }, scn::observed::construct,
			[](ecs::bus_context& ctx, entt::entity entity) {
				BOOST_TEST(ctx.world() == "w0");
				g_constructed.push_back(ctx.registry().get<module_marker>(entity).value);
			});
		context.observe<module_marker>({ .set = "level" }, scn::observed::destroy, [](ecs::bus_context&, entt::entity) { ++g_destroyed; });
	}
};

constexpr const char* LEVEL = R"json({"__type":"level_desc","name":"lv","worlds":[{"__type":"world_desc","name":"w0","systems":[]}]})json";

struct observer_module_fixture : test_util::module_env
{
	observer_module_fixture()
	{
		root.attach(test_util::borrowed(tasks));
		g_constructed.clear();
		g_destroyed = 0;
		g_construct_connection = nullptr;
	}

	void load_level()
	{
		BOOST_REQUIRE(contexts().load_level(put("lv.desc", LEVEL)));
		pump();
	}

	entt::registry& world() { return contexts().level_set().get("w0").state(); }

	entt::entity mark(int value)
	{
		entt::registry& reg = world();
		const entt::entity entity = reg.create();
		reg.emplace<module_marker>(entity, module_marker{ value });
		return entity;
	}
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(GsModuleObserverTests, observer_module_fixture)

BOOST_AUTO_TEST_CASE(AModulesObserverIsKeptByItsLayerAndHearsItsWorlds)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_TEST(layer->subscriptions().size() == 2u);
	BOOST_TEST(layer->kept_count() == 2u);

	load_level();
	const entt::entity first = mark(1);
	BOOST_TEST((g_constructed == std::vector<int>{ 1 }));
	world().destroy(first);
	BOOST_TEST(g_destroyed == 1);

	// A level reload builds new worlds: they hear it too.
	load_level();
	mark(2);
	BOOST_TEST((g_constructed == std::vector<int>{ 1, 2 }));
}

BOOST_AUTO_TEST_CASE(MutedObserversHearNothing_AndGuardsNest)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	load_level();
	auto& subscriptions = contexts().game_layer()->subscriptions();
	{
		const auto outer = subscriptions.mute_observers();
		{
			const auto inner = subscriptions.mute_observers();
			mark(1);
		}
		BOOST_TEST(subscriptions.observers_muted());
		mark(2);
	}
	BOOST_TEST(!subscriptions.observers_muted());
	BOOST_TEST(g_constructed.empty());
	mark(3);
	BOOST_TEST((g_constructed == std::vector<int>{ 3 }));
}

// 2b keeps the guard for the length of a restore that spans calls: it has to move into a member or an optional.
BOOST_AUTO_TEST_CASE(AMovedMuteGuardKeepsTheMuteUntilItIsReleased)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	load_level();
	auto& subscriptions = contexts().game_layer()->subscriptions();
	std::optional<scn::event_subscriptions::mute_guard> held;
	{
		auto guard = subscriptions.mute_observers();
		held.emplace(std::move(guard));
	}
	BOOST_TEST(subscriptions.observers_muted());
	mark(1);
	scn::event_subscriptions::mute_guard moved_again = std::move(*held);
	held.reset(); // a moved-from guard unmutes nothing
	BOOST_TEST(subscriptions.observers_muted());
	moved_again.release();
	BOOST_TEST(!subscriptions.observers_muted());
	mark(2);
	BOOST_TEST((g_constructed == std::vector<int>{ 2 }));
}

// As a bus subscriber: nothing while the set is in Edit (a level load, an editor edit).
BOOST_AUTO_TEST_CASE(AnObserverHearsNothingInEdit)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	load_level();
	contexts().level_set().set_mode(ecs::run_mode::edit);
	mark(1);
	contexts().level_set().set_mode(ecs::run_mode::play);
	mark(2);
	BOOST_TEST((g_constructed == std::vector<int>{ 2 }));
}

// One observed after the world exists is connected at the world's next update; on_update is heard like the others.
BOOST_AUTO_TEST_CASE(AnObserverAddedToALiveWorldIsConnectedAtItsNextUpdate)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	load_level();
	std::vector<int> updated;
	const ecs::connection observer = contexts().game_layer()->subscriptions().observe<module_marker>({ .set = "level" }, scn::observed::update,
		[&updated](ecs::bus_context& ctx, entt::entity entity) { updated.push_back(ctx.registry().get<module_marker>(entity).value); });
	const entt::entity entity = mark(1);
	world().patch<module_marker>(entity, [](module_marker& marker) { marker.value = 10; });
	BOOST_TEST(updated.empty());
	contexts().update(std::chrono::duration<float>(0.016f));
	world().patch<module_marker>(entity, [](module_marker& marker) { marker.value = 11; });
	BOOST_TEST((updated == std::vector<int>{ 11 }));
}

BOOST_AUTO_TEST_CASE(ADisconnectedObserverIsSilentInEveryWorld)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	load_level();
	mark(1);
	BOOST_REQUIRE(g_construct_connection != nullptr);
	g_construct_connection->disconnect();
	mark(2);
	contexts().update(std::chrono::duration<float>(0.016f));
	mark(3);
	BOOST_TEST((g_constructed == std::vector<int>{ 1 }));
}

// The Lead's review condition for 2a: after the unload nothing of the module is called, also in the worlds that follow.
BOOST_AUTO_TEST_CASE(UnloadingTheModuleRemovesItsObservers)
{
	loader.load(std::make_unique<observing_game_module>(), root);
	load_level();
	mark(1);
	loader.unload("observing_game", root);

	auto* fresh = contexts().game_layer();
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(fresh->subscriptions().size() == 0u);
	BOOST_TEST(contexts().level_set().size() == 0u); // its worlds went with its layer
	const int destroyed_by_unload = g_destroyed;

	load_level();
	world().destroy(mark(2));
	BOOST_TEST((g_constructed == std::vector<int>{ 1 }));
	BOOST_TEST(g_destroyed == destroyed_by_unload);
}

BOOST_AUTO_TEST_SUITE_END()
