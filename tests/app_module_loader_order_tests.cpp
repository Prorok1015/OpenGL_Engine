#include <boost/test/unit_test.hpp>
#include "app_module_loader.hpp"
#include <string>
#include <vector>

// Order guarantees of the module loader: dependencies first, priority/insertion order only as
// tiebreakers, shutdown in exact reverse. (Cycle / unknown-dependency detection is guarded by
// ASSERT_MSG, which is compiled out in this configuration, so it cannot be asserted here.)

namespace {

using app::modules::MODULE_PRIORITY;

std::vector<std::string> g_events;

struct test_module : app::modules::module_interface
{
	test_module(std::string n, MODULE_PRIORITY p = MODULE_PRIORITY::CORE, std::vector<std::string_view> deps = {})
		: m_name(std::move(n)), m_priority(p), m_deps(std::move(deps)) {}

	void register_services(ds::app_data_storage&) override { g_events.push_back("register:" + m_name); }
	void initialize_services(ds::app_data_storage&) override { g_events.push_back("init:" + m_name); }
	void shutdown_services(ds::app_data_storage&) override { g_events.push_back("shutdown:" + m_name); }
	MODULE_PRIORITY get_priority() const override { return m_priority; }
	std::string_view name() const override { return m_name; }
	std::vector<std::string_view> depends_on() const override { return m_deps; }

	std::string m_name;
	MODULE_PRIORITY m_priority;
	std::vector<std::string_view> m_deps;
};

// names of the given phase in the order they ran
std::vector<std::string> phase(const std::string& prefix)
{
	std::vector<std::string> out;
	for (const auto& e : g_events) {
		if (e.rfind(prefix, 0) == 0) out.push_back(e.substr(prefix.size()));
	}
	return out;
}

struct loader_fixture
{
	loader_fixture() { g_events.clear(); }

	void add(std::string n, MODULE_PRIORITY p = MODULE_PRIORITY::CORE, std::vector<std::string_view> deps = {})
	{
		loader.add_module(std::make_unique<test_module>(std::move(n), p, std::move(deps)));
	}

	app::modules::module_loader loader;
	ds::app_data_storage storage;
};

using names = std::vector<std::string>;

}

BOOST_FIXTURE_TEST_SUITE(AppModuleLoaderTests, loader_fixture)

BOOST_AUTO_TEST_CASE(DependenciesRunBeforeDependentsEvenWhenAddedLater)
{
	add("game", MODULE_PRIORITY::GAME, { "engine" });
	add("engine", MODULE_PRIORITY::ENGINE, { "core" });
	add("core", MODULE_PRIORITY::CORE);

	loader.register_all_services(storage);
	BOOST_TEST((phase("register:") == names{ "core", "engine", "game" }));
}

BOOST_AUTO_TEST_CASE(DependencyBeatsLowerPriorityValue)
{
	// "early" has the lowest priority value but depends on "late"; the dependency still wins.
	add("early", MODULE_PRIORITY::CORE, { "late" });
	add("late", MODULE_PRIORITY::GAME);

	loader.initialize_all_services(storage);
	BOOST_TEST((phase("init:") == names{ "late", "early" }));
}

BOOST_AUTO_TEST_CASE(IndependentModulesOrderByPriorityThenInsertion)
{
	add("editor1", MODULE_PRIORITY::EDITOR);
	add("game1", MODULE_PRIORITY::GAME);
	add("core1", MODULE_PRIORITY::CORE);
	add("editor2", MODULE_PRIORITY::EDITOR);
	add("core2", MODULE_PRIORITY::CORE);

	loader.register_all_services(storage);
	BOOST_TEST((phase("register:") == names{ "core1", "core2", "editor1", "editor2", "game1" }));
}

BOOST_AUTO_TEST_CASE(DiamondDependencyIsOrderedAndEachModuleRunsOnce)
{
	add("top", MODULE_PRIORITY::CORE, { "left", "right" });
	add("left", MODULE_PRIORITY::CORE, { "base" });
	add("right", MODULE_PRIORITY::CORE, { "base" });
	add("base", MODULE_PRIORITY::CORE);

	loader.register_all_services(storage);
	auto order = phase("register:");
	BOOST_REQUIRE(order.size() == 4u);
	BOOST_TEST(order.front() == "base");
	BOOST_TEST(order.back() == "top");
	BOOST_TEST((order[1] == "left" && order[2] == "right")); // insertion order breaks the tie
}

BOOST_AUTO_TEST_CASE(RegisterInitializeAndShutdownShareOneConsistentOrder)
{
	add("b", MODULE_PRIORITY::ENGINE, { "a" });
	add("a", MODULE_PRIORITY::CORE);
	add("c", MODULE_PRIORITY::GAME, { "b" });

	loader.register_all_services(storage);
	loader.initialize_all_services(storage);
	loader.shutdown_all_services(storage);

	BOOST_TEST((phase("register:") == names{ "a", "b", "c" }));
	BOOST_TEST((phase("init:") == names{ "a", "b", "c" }));
	BOOST_TEST((phase("shutdown:") == names{ "c", "b", "a" })); // exact reverse
}

BOOST_AUTO_TEST_CASE(ModuleAddedAfterFirstUseIsSortedIntoTheNextPass)
{
	add("b", MODULE_PRIORITY::ENGINE, { "a" });
	add("a", MODULE_PRIORITY::CORE);
	loader.register_all_services(storage);

	g_events.clear();
	add("c", MODULE_PRIORITY::CORE, { "b" });
	loader.initialize_all_services(storage);
	BOOST_TEST((phase("init:") == names{ "a", "b", "c" }));
}

BOOST_AUTO_TEST_CASE(EmptyLoaderIsANoOp)
{
	BOOST_CHECK_NO_THROW(loader.register_all_services(storage));
	BOOST_CHECK_NO_THROW(loader.initialize_all_services(storage));
	BOOST_CHECK_NO_THROW(loader.shutdown_all_services(storage));
	BOOST_TEST(g_events.empty());
}

BOOST_AUTO_TEST_SUITE_END()
