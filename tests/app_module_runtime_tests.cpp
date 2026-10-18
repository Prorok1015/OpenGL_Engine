#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_loader.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// load() / unload() of a single module after startup.

namespace {

using app::modules::module_state;
using deps_t = std::vector<std::string_view>;

struct svc_a { int v = 1; };
struct svc_b { int v = 2; };
struct svc_c { int v = 3; };

template <class Service>
struct service_module : app::modules::module_interface
{
	service_module(std::string n, deps_t deps = {}, bool leak = false)
		: m_name(std::move(n)), m_deps(std::move(deps)), m_leak(leak) {}

	void register_services(ds::app_data_storage& data) override { data.construct<Service>(); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage& data) override
	{
		if (!m_leak) {
			data.destruct<Service>();
		}
	}
	std::string_view name() const override { return m_name; }
	deps_t depends_on() const override { return m_deps; }

	std::string m_name;
	deps_t m_deps;
	bool m_leak;
};

template <class Service>
std::unique_ptr<app::modules::module_interface> make(std::string n, deps_t deps = {}, bool leak = false)
{
	return std::make_unique<service_module<Service>>(std::move(n), std::move(deps), leak);
}

struct runtime_fixture
{
	runtime_fixture()
	{
		loader.add_module(make<svc_a>("a"));
		loader.register_all_services(storage);
		loader.initialize_all_services(storage);
	}

	~runtime_fixture()
	{
		try { loader.shutdown_all_services(storage); } catch (...) {}
	}

	app::modules::module_loader loader;
	ds::app_data_storage storage;
};

}

BOOST_FIXTURE_TEST_SUITE(AppModuleRuntimeTests, runtime_fixture)

BOOST_AUTO_TEST_CASE(LoadRegistersAndInitializesOnlyTheNewModule)
{
	BOOST_REQUIRE_EQUAL(storage.size(), 1u);
	loader.load(make<svc_b>("b", { "a" }), storage);

	BOOST_TEST(storage.size() == 2u);
	BOOST_TEST((loader.state("b") == module_state::initialized));
	BOOST_TEST((loader.state("a") == module_state::initialized));
}

BOOST_AUTO_TEST_CASE(LoadWithMissingDependencyIsRefusedAndRegistersNothing)
{
	BOOST_CHECK_THROW(loader.load(make<svc_b>("b", { "nope" }), storage), std::logic_error);
	BOOST_TEST(storage.size() == 1u);
	BOOST_TEST(!loader.state("b").has_value());
}

BOOST_AUTO_TEST_CASE(LoadWithUninitializedDependencyIsRefused)
{
	loader.add_module(make<svc_c>("pending")); // added, never initialized
	BOOST_CHECK_THROW(loader.load(make<svc_b>("b", { "pending" }), storage), std::logic_error);
	BOOST_TEST(storage.size() == 1u);
	BOOST_TEST((loader.state("pending") == module_state::added));
}

BOOST_AUTO_TEST_CASE(LoadWithDuplicateNameIsRefused)
{
	BOOST_CHECK_THROW(loader.load(make<svc_b>("a"), storage), std::logic_error);
	BOOST_TEST(storage.size() == 1u);
}

BOOST_AUTO_TEST_CASE(UnloadShutsDownRemovesAndReportsUnloaded)
{
	loader.load(make<svc_b>("b", { "a" }), storage);
	loader.unload("b", storage);

	BOOST_TEST(storage.size() == 1u);
	BOOST_TEST((loader.state("b") == module_state::unloaded));
	BOOST_TEST(!storage.has_value<svc_b>());
}

BOOST_AUTO_TEST_CASE(UnloadedModuleCanBeLoadedAgain)
{
	loader.load(make<svc_b>("b"), storage);
	loader.unload("b", storage);
	loader.load(make<svc_b>("b"), storage);
	BOOST_TEST((loader.state("b") == module_state::initialized));
	BOOST_TEST(storage.size() == 2u);
}

BOOST_AUTO_TEST_CASE(UnloadIsRefusedWhileAnInitializedModuleDependsOnIt)
{
	loader.load(make<svc_b>("b", { "a" }), storage);

	BOOST_CHECK_THROW(loader.unload("a", storage), std::logic_error);
	BOOST_TEST((loader.state("a") == module_state::initialized));
	BOOST_TEST(storage.size() == 2u); // nothing was shut down

	loader.unload("b", storage);
	BOOST_CHECK_NO_THROW(loader.unload("a", storage));
	BOOST_TEST(storage.size() == 0u);
}

BOOST_AUTO_TEST_CASE(UnloadOfUnknownOrNotInitializedModuleIsRefused)
{
	BOOST_CHECK_THROW(loader.unload("ghost", storage), std::logic_error);
	loader.add_module(make<svc_c>("pending"));
	BOOST_CHECK_THROW(loader.unload("pending", storage), std::logic_error);
}

BOOST_AUTO_TEST_CASE(UnloadDetectsAServiceTheModuleForgotToDestruct)
{
	loader.load(make<svc_b>("leaky", {}, true), storage);

	BOOST_CHECK_THROW(loader.unload("leaky", storage), std::logic_error);
	BOOST_TEST((loader.state("leaky") == module_state::unloaded)); // removed anyway, the leak is reported
	BOOST_TEST(storage.size() == 2u);                               // a + the leaked svc_b
	storage.destruct<svc_b>();
}

BOOST_AUTO_TEST_CASE(UnloadingAMiddleModuleIsNotConfusedByLaterModules)
{
	loader.load(make<svc_b>("b"), storage);
	loader.load(make<svc_c>("c"), storage);

	BOOST_CHECK_NO_THROW(loader.unload("b", storage)); // c registered after b and stays
	BOOST_TEST(storage.size() == 2u);
	BOOST_TEST(storage.has_value<svc_c>());
}

BOOST_AUTO_TEST_CASE(FailedInitializeDuringLoadIsRolledBackAndCanBeRetried)
{
	bool fail_init = true;
	struct flaky_module final : service_module<svc_b>
	{
		flaky_module(bool& f) : service_module<svc_b>("flaky"), m_fail(f) {}
		void initialize_services(ds::app_data_storage&) override
		{
			if (m_fail) {
				throw std::runtime_error("init failed");
			}
		}
		bool& m_fail;
	};

	BOOST_CHECK_THROW(loader.load(std::make_unique<flaky_module>(fail_init), storage), std::runtime_error);
	BOOST_TEST(storage.size() == 1u); // svc_b registered by the module was torn down again
	BOOST_TEST(!loader.state("flaky").has_value());

	fail_init = false;
	BOOST_CHECK_NO_THROW(loader.load(std::make_unique<flaky_module>(fail_init), storage));
	BOOST_TEST((loader.state("flaky") == module_state::initialized));
	BOOST_TEST(storage.size() == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
