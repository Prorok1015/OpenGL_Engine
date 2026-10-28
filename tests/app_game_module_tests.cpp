#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/config/cfg_variables.h>
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_component.h>
#include <snk/ecs/ecs_system.h>
#include <snk/game/gs_game_module.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <entt/entt.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// A game module in the same binary (#220 PR-6, MODULE_LAYER.md): module_loader gives it a layer, it registers a desc
// type with its spawner, a system, a component's meta, a message type and a service of its own there, keeps its
// registrations in base registries and its tasks in the layer, and unloading it leaves the root storage and the base
// registries as they were.

namespace {

struct game_component {
	int value = 0;
};

struct base_component {
	int ticks = 0;
};

struct game_message {
	int value = 0;
};

struct game_desc : desc::desc_base {
	int value = 0;

	void deserialize(desc::desc_system&, const boost::json::object& obj) override
	{
		if (const auto* v = obj.if_contains("value")) value = static_cast<int>(v->to_number<std::int64_t>());
	}
	void serialize(boost::json::object& obj) const override { obj["value"] = value; }
};

// What the module did, for the tests to look at.
std::vector<std::string> g_teardown;
bool g_task_ran = false;
std::optional<ds::task_handle<void>> g_task;

struct game_service {
	~game_service() { g_teardown.emplace_back("service"); }
	int frames = 0;
};

void game_move(entt::view<entt::get_t<game_component>> view)
{
	for (auto&& [entity, component] : view.each()) {
		++component.value;
	}
}

// One instance per world that runs it: destroyed with the world.
struct game_world_system : ecs::system_interface {
	~game_world_system() override { g_teardown.emplace_back("world"); }
	void run() {}
};

void base_tick(entt::view<entt::get_t<base_component>> view)
{
	for (auto&& [entity, component] : view.each()) {
		++component.ticks;
	}
}

class test_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "test_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		layer.descs().register_component_desc<game_desc>("game_desc");
		layer.assembler().register_desc_spawner("game_desc", [](entt::registry& reg, entt::entity e, const desc::desc_base& base, const std::string_view) {
			reg.emplace_or_replace<game_component>(e, game_component{ static_cast<const game_desc&>(base).value });
		});
		layer.systems().register_system<&game_move>("game::move");
		layer.systems().register_system<&game_world_system::run, game_world_system>("game::world", {});
		layer.components().add<game_component>("game.component");
		layer.channels().add<game_message>();
		ecs::register_component<game_component>(layer.meta());
		data.construct<game_service>();

		auto& context = data.require<gs::module_context>();
		cmd::command_desc ping;
		ping.id = "game.ping";
		ping.summary = "Answers.";
		ping.invoke = [](const cmd::call&) { return cmd::result::success(); };
		context.add_command(std::move(ping));
		context.add_log_sink([](std::string_view, engine::log_level, std::string_view) {});
		g_task = context.run(ds::lane::compute, ds::task_desc{ .name = "game.background" }, [](ds::task_context&) { g_task_ran = true; });
	}
	// game_service stays in the layer's storage: it goes after the layer.
};

class other_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "other_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		layer.descs().register_component_desc<game_desc>("other_desc");
	}
};

bool g_throwing_shut_down = false;

// What the module's storage gives it of the base.
struct probe_result {
	bool commands_visible = true;
	bool tasks_visible = true;
	bool contexts_visible = true;
	bool require_refused = false;
	bool own_layer_visible = false;
	std::optional<std::string> config_value;
	bool own_store_written = false;
	bool base_store_refused = false;
	bool base_erase_refused = false;
};

// Set by the base before the probing module loads.
constexpr const char* PROBE_VARIABLE = "gametest/probe";
const res::tag PROBE_BASE_TAG{ "memory://t/base.txt" };

// The module only reads the configuration.
static_assert(std::is_same_v<decltype(std::declval<const gs::module_context&>().config()), const cfg::config_view&>);
probe_result g_probe;

class probing_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "probing_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer&, ds::app_data_storage& data) override
	{
		g_probe.commands_visible = data.has_value<cmd::command_registry>();
		g_probe.tasks_visible = data.has_value<ds::task_system>();
		g_probe.contexts_visible = data.has_value<scn::context_manager>();
		cmd::command_desc sneaky;
		sneaky.id = "game.sneaky";
		sneaky.invoke = [](const cmd::call&) { return cmd::result::success(); };
		try {
			m_kept = data.require<cmd::command_registry>().add(std::move(sneaky));
		} catch (const std::logic_error&) {
			g_probe.require_refused = true;
		}
		g_probe.own_layer_visible = data.has_value<scn::scene_layer>() && data.has_value<gs::module_context>();

		g_probe.config_value = data.require<gs::module_context>().config().value(PROBE_VARIABLE);
		auto& resources = data.require<res::resource_system>();
		g_probe.own_store_written = resources.store(res::tag{ gs::module_resource_prefix(name()) + "own.txt" }, test_util::to_bytes("own"));
		try {
			resources.store(PROBE_BASE_TAG, test_util::to_bytes("changed"));
		} catch (const std::logic_error&) {
			g_probe.base_store_refused = true;
		}
		try {
			resources.erase(PROBE_BASE_TAG);
		} catch (const std::logic_error&) {
			g_probe.base_erase_refused = true;
		}
	}

private:
	cmd::registration m_kept;
};

// Continuations of the module's task, with and without an owner; their captures stand for the module's code (#406).
std::weak_ptr<int> g_plain_capture;
std::weak_ptr<int> g_owned_capture;
int g_plain_calls = 0;

class continuing_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "continuing_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage& data) override
	{
		auto task = data.require<gs::module_context>().run(ds::lane::compute, ds::task_desc{ .name = "game.continued" },
			[](ds::task_context&) {});
		auto plain = std::make_shared<int>(0);
		g_plain_capture = plain;
		task.then_main([plain](ds::task_result<void>&) { ++g_plain_calls; });
		auto owned = std::make_shared<int>(0);
		g_owned_capture = owned;
		task.then_main(layer.tasks(), [owned](ds::task_result<void>&) {});
	}
};

class throwing_game_module final : public gs::game_module {
public:
	std::string_view name() const override { return "throwing_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		layer.descs().register_component_desc<game_desc>("throwing_desc");
		throw std::runtime_error("registration failed");
	}
	void shutdown_content(scn::scene_layer&, ds::app_data_storage&) override { g_throwing_shut_down = true; }
};

constexpr const char* GAME_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["game::move","game::world"],
	 "components":{"g":{"__type":"game_desc","value":2}}}]})";

// A world of the module's layer with a prefab instance: the layer's hot reload manager watches the prefab.
constexpr const char* PREFAB_LEVEL = R"({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":["game::move"],
	 "components":{"g":{"__type":"game_desc","value":1}},
	 "children":{"robot":{"components":{"model_key":"memory://t/rig.prefab.desc"}}}}]})";

constexpr const char* RIG_PREFAB = R"({"__type":"prefab_desc","__version":3,"children":[
	{"__id":"00000000000000a1","name":"bone"}]})";

struct game_module_fixture : test_util::module_env {
	game_module_fixture()
	{
		root.attach(test_util::borrowed(tasks));
		g_teardown.clear();
		g_task_ran = false;
		g_task.reset();
		g_throwing_shut_down = false;
		g_probe = {};
	}
};

}

BOOST_FIXTURE_TEST_SUITE(app_game_module_tests, game_module_fixture)

BOOST_AUTO_TEST_CASE(a_game_module_registers_into_its_layer_and_its_worlds_run)
{
	const std::size_t root_size = root.size();
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_TEST(root.size() == root_size); // its service is in the layer's storage

	auto* layer = contexts().game_layer();
	BOOST_REQUIRE(layer != nullptr);
	BOOST_TEST(layer->descs().is_registered("game_desc"));
	BOOST_TEST(!descs->is_registered("game_desc"));
	BOOST_TEST(!sf.registry().find("game::move"));
	BOOST_TEST(!root.has_value<game_service>());
	BOOST_TEST(layer->channels().contains<game_message>());
	BOOST_TEST(!contexts().channels().contains<game_message>());

	BOOST_REQUIRE(contexts().load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	auto view = world->state().view<game_component>();
	BOOST_REQUIRE(view.size() == 1u);
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(view.get<game_component>(view.front()).value == 3);
}

BOOST_AUTO_TEST_CASE(unloading_leaves_the_root_and_the_base_as_they_were)
{
	const std::size_t root_size = root.size();
	const auto base_components = descs->get_component_type_names();
	const std::size_t base_systems = sf.registry().size();
	const std::size_t base_commands = commands.size();
	const std::size_t base_channels = contexts().channels().size();
	const std::size_t base_sinks = engine::log_sink_count();

	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_REQUIRE(contexts().load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	BOOST_TEST(commands.size() == base_commands + 1u);
	BOOST_TEST(engine::log_sink_count() == base_sinks + 1u);
	BOOST_TEST(contexts().game_layer()->kept_count() == 2u);

	loader.unload("test_game", root);
	BOOST_TEST(root.size() == root_size);
	BOOST_TEST((loader.state("test_game") == app::modules::module_state::unloaded));
	BOOST_TEST(descs->get_component_type_names() == base_components, boost::test_tools::per_element());
	BOOST_TEST(!assembler->has_spawner("game_desc"));
	BOOST_TEST(sf.registry().size() == base_systems);
	BOOST_TEST(commands.size() == base_commands);
	BOOST_TEST(contexts().channels().size() == base_channels);
	BOOST_TEST(engine::log_sink_count() == base_sinks);

	// A fresh, empty game layer is there for the editor; it knows nothing of the module.
	auto* fresh = contexts().game_layer();
	BOOST_REQUIRE(fresh != nullptr);
	BOOST_TEST(!fresh->descs().is_registered("game_desc"));
	BOOST_TEST(contexts().level_set().size() == 0u);
}

BOOST_AUTO_TEST_CASE(the_module_tasks_are_stopped_before_its_layer_goes)
{
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_REQUIRE(g_task.has_value());
	loader.unload("test_game", root);
	BOOST_TEST(g_task->done());
	BOOST_TEST((g_task->status() == ds::task_status::cancelled));
	BOOST_TEST(!g_task_ran);
}

// Stopping the module's tasks queues their continuations; the unload runs them while the layer is there, so nothing
// of the module's code waits in the main queue for the next frame, after its source is closed.
BOOST_AUTO_TEST_CASE(unloading_runs_and_drops_the_continuations_of_the_module_tasks)
{
	g_plain_calls = 0;
	loader.load(std::make_unique<continuing_game_module>(), root);
	BOOST_REQUIRE(!g_plain_capture.expired());
	BOOST_REQUIRE(!g_owned_capture.expired());
	loader.unload("continuing_game", root);
	BOOST_TEST(g_plain_calls == 1);
	BOOST_TEST(g_plain_capture.expired());
	BOOST_TEST(g_owned_capture.expired());
}

BOOST_AUTO_TEST_CASE(unloading_takes_the_worlds_then_the_layer_then_the_module_services)
{
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_REQUIRE(contexts().load_level(put("lv.desc", GAME_LEVEL)));
	pump();
	g_teardown.clear();

	loader.unload("test_game", root);
	BOOST_TEST(g_teardown == (std::vector<std::string>{ "world", "service" }), boost::test_tools::per_element());
}

// Under ASan: unloading must not reach the layer's hot reload manager after the layer's resources are gone.
BOOST_AUTO_TEST_CASE(unloading_a_module_whose_world_has_a_prefab_instance)
{
	put("rig.prefab.desc", RIG_PREFAB);
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_REQUIRE(contexts().load_level(put("lv.desc", PREFAB_LEVEL)));
	pump();
	auto* world = contexts().level_set().find("w0");
	BOOST_REQUIRE(world != nullptr);
	BOOST_TEST(!world->state().view<scn::prefab_instance>().empty());

	loader.unload("test_game", root);
	BOOST_TEST(contexts().level_set().size() == 0u);
	BOOST_TEST(!contexts().game_layer()->descs().is_registered("game_desc"));
}

BOOST_AUTO_TEST_CASE(the_game_layer_moves_to_the_module_only_after_it_registered)
{
	auto* empty = contexts().game_layer();
	loader.load(std::make_unique<test_game_module>(), root);
	BOOST_TEST(contexts().game_layer() != empty);
	BOOST_TEST(contexts().game_layer()->descs().is_registered("game_desc"));

	// A module that fails to register never becomes the game layer.
	loader.unload("test_game", root);
	auto* fresh = contexts().game_layer();
	BOOST_CHECK_THROW(loader.load(std::make_unique<throwing_game_module>(), root), std::runtime_error);
	BOOST_TEST(contexts().game_layer() == fresh);
}

BOOST_AUTO_TEST_CASE(the_module_loads_again_after_unloading)
{
	for (int round = 0; round < 2; ++round) {
		BOOST_TEST_CONTEXT("round " << round) {
			loader.load(std::make_unique<test_game_module>(), root);
			BOOST_TEST(contexts().game_layer()->descs().is_registered("game_desc"));
			BOOST_TEST((contexts().game_layer()->components().find("game.component") == entt::type_hash<game_component>::value()));
			loader.unload("test_game", root);
		}
	}
}

BOOST_AUTO_TEST_CASE(a_second_game_module_is_refused)
{
	loader.load(std::make_unique<test_game_module>(), root);
	auto* first = contexts().game_layer();

	BOOST_CHECK_THROW(loader.load(std::make_unique<other_game_module>(), root), std::logic_error);
	BOOST_TEST(!loader.state("other_game").has_value());
	BOOST_TEST(contexts().game_layer() == first); // the first module's layer is untouched
	BOOST_TEST(first->descs().is_registered("game_desc"));

	loader.unload("test_game", root);
	loader.load(std::make_unique<other_game_module>(), root);
	BOOST_TEST(contexts().game_layer()->descs().is_registered("other_desc"));
	loader.unload("other_game", root);
}

BOOST_AUTO_TEST_CASE(a_module_whose_registration_throws_is_shut_down_and_its_layer_dropped)
{
	const std::size_t root_size = root.size();
	BOOST_CHECK_THROW(loader.load(std::make_unique<throwing_game_module>(), root), std::runtime_error);
	BOOST_TEST(g_throwing_shut_down);
	BOOST_TEST(!loader.state("throwing_game").has_value());
	BOOST_TEST(root.size() == root_size);
	BOOST_REQUIRE(contexts().game_layer() != nullptr);
	BOOST_TEST(!contexts().game_layer()->descs().is_registered("throwing_desc"));

	loader.load(std::make_unique<test_game_module>(), root); // the factory is free again
	loader.unload("test_game", root);
}

// The base sets are built from the base registries alone, and the module's sets go with its layer: no plan refers
// to the module, so unloading it needs no rebuild_all_plans.
BOOST_AUTO_TEST_CASE(base_sets_keep_running_without_a_plan_rebuild)
{
	sf.register_system<&base_tick>("base::tick", { .run_in_edit = true });
	contexts().create_set("base");
	auto& world = contexts().create_context("base", "bw", scn::context_kind::runtime, { "base::tick" });
	const auto entity = world.state().create();
	world.state().emplace<base_component>(entity);

	contexts().update(std::chrono::duration<float>(0.016f));
	const int before = world.state().get<base_component>(entity).ticks;
	BOOST_REQUIRE(before > 0);

	loader.load(std::make_unique<test_game_module>(), root);
	loader.unload("test_game", root);
	contexts().update(std::chrono::duration<float>(0.016f));
	BOOST_TEST(world.state().get<base_component>(entity).ticks > before);
}

BOOST_AUTO_TEST_CASE(a_module_cannot_reach_the_base_registries_directly)
{
	const std::size_t base_commands = commands.size();
	cfg::variable<int> probe{ PROBE_VARIABLE, 7 };
	BOOST_REQUIRE(rs->store(PROBE_BASE_TAG, test_util::to_bytes("base")));
	loader.load(std::make_unique<probing_game_module>(), root);
	BOOST_TEST(g_probe.config_value.value_or("") == "7");
	BOOST_TEST(g_probe.own_store_written);
	BOOST_TEST(g_probe.base_store_refused);
	BOOST_TEST(g_probe.base_erase_refused);
	BOOST_TEST(rs->exists(PROBE_BASE_TAG));
	BOOST_TEST(!g_probe.commands_visible);
	BOOST_TEST(!g_probe.tasks_visible);
	BOOST_TEST(!g_probe.contexts_visible);
	BOOST_TEST(g_probe.require_refused);
	BOOST_TEST(g_probe.own_layer_visible);
	BOOST_TEST(commands.size() == base_commands);
	loader.unload("probing_game", root);
}

BOOST_AUTO_TEST_CASE(a_layered_module_needs_a_layer_factory)
{
	root.destruct<app::modules::module_layer_factory>();
	BOOST_CHECK_THROW(loader.load(std::make_unique<test_game_module>(), root), std::logic_error);
	BOOST_TEST(!loader.state("test_game").has_value());
}

BOOST_AUTO_TEST_SUITE_END()
