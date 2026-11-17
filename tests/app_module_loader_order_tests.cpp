#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_loader.hpp>
#include <string>
#include <vector>

// Order guarantees of the module loader: dependencies first, insertion order as the only
// tiebreaker, shutdown in exact reverse. (Cycle / unknown-dependency detection is guarded by
// ASSERT_MSG, which is compiled out in this configuration, so it cannot be asserted here.)

namespace {

std::vector<std::string> g_events;

struct test_module : app::modules::module_interface
{
	test_module(std::string n, std::vector<std::string_view> deps = {})
		: m_name(std::move(n)), m_deps(std::move(deps)) {}

	void register_services(ds::app_data_storage&) override { g_events.push_back("register:" + m_name); }
	void initialize_services(ds::app_data_storage&) override { g_events.push_back("init:" + m_name); }
	void shutdown_services(ds::app_data_storage&) override { g_events.push_back("shutdown:" + m_name); }
	std::string_view name() const override { return m_name; }
	std::vector<std::string_view> depends_on() const override { return m_deps; }

	std::string m_name;
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

	void add(std::string n, std::vector<std::string_view> deps = {})
	{
		loader.add_module(std::make_unique<test_module>(std::move(n), std::move(deps)));
	}

	app::modules::module_loader loader;
	ds::app_data_storage storage;
};

using names = std::vector<std::string>;

}

BOOST_FIXTURE_TEST_SUITE(AppModuleLoaderTests, loader_fixture)

BOOST_AUTO_TEST_CASE(DependenciesRunBeforeDependentsEvenWhenAddedLater)
{
	add("game", { "engine" });
	add("engine", { "core" });
	add("core");

	loader.register_all_services(storage);
	BOOST_TEST((phase("register:") == names{ "core", "engine", "game" }));
}

BOOST_AUTO_TEST_CASE(DependencyBeatsInsertionOrder)
{
	// "early" is inserted first but depends on "late"; the dependency still wins.
	add("early", { "late" });
	add("late");

	loader.initialize_all_services(storage);
	BOOST_TEST((phase("init:") == names{ "late", "early" }));
}

BOOST_AUTO_TEST_CASE(IndependentModulesKeepInsertionOrder)
{
	add("editor1");
	add("game1");
	add("core1");
	add("editor2");
	add("core2");

	loader.register_all_services(storage);
	BOOST_TEST((phase("register:") == names{ "editor1", "game1", "core1", "editor2", "core2" }));
}

BOOST_AUTO_TEST_CASE(DiamondDependencyIsOrderedAndEachModuleRunsOnce)
{
	add("top", { "left", "right" });
	add("left", { "base" });
	add("right", { "base" });
	add("base");

	loader.register_all_services(storage);
	auto order = phase("register:");
	BOOST_REQUIRE(order.size() == 4u);
	BOOST_TEST(order.front() == "base");
	BOOST_TEST(order.back() == "top");
	BOOST_TEST((order[1] == "left" && order[2] == "right")); // insertion order breaks the tie
}

BOOST_AUTO_TEST_CASE(RegisterInitializeAndShutdownShareOneConsistentOrder)
{
	add("b", { "a" });
	add("a");
	add("c", { "b" });

	loader.register_all_services(storage);
	loader.initialize_all_services(storage);
	loader.shutdown_all_services(storage);

	BOOST_TEST((phase("register:") == names{ "a", "b", "c" }));
	BOOST_TEST((phase("init:") == names{ "a", "b", "c" }));
	BOOST_TEST((phase("shutdown:") == names{ "c", "b", "a" })); // exact reverse
}

BOOST_AUTO_TEST_CASE(ModuleAddedAfterFirstUseIsSortedIntoTheNextPass)
{
	add("b", { "a" });
	add("a");
	loader.register_all_services(storage);

	g_events.clear();
	add("c", { "b" });
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

BOOST_AUTO_TEST_CASE(StatesFollowTheLifecycle)
{
	using app::modules::module_state;
	add("a");
	add("b", { "a" });
	BOOST_TEST((loader.state("a") == module_state::added));
	BOOST_TEST(!loader.state("zzz").has_value());

	loader.register_all_services(storage);
	BOOST_TEST((loader.state("b") == module_state::registered));
	loader.initialize_all_services(storage);
	BOOST_TEST((loader.state("a") == module_state::initialized));
	BOOST_TEST((loader.state("b") == module_state::initialized));
	loader.shutdown_all_services(storage);
	BOOST_TEST((loader.state("a") == module_state::shut_down));
	BOOST_TEST((loader.state("b") == module_state::shut_down));
}

BOOST_AUTO_TEST_SUITE_END()
