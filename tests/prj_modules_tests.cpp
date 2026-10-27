#include <boost/test/unit_test.hpp>
#include <snk/project/prj_modules.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct game_state
{
	int value = 0;
};

class fake_module : public app::modules::module_interface
{
public:
	fake_module(std::string name, bool throw_on_init, std::vector<std::string_view> deps)
		: m_name(std::move(name))
		, m_throw_on_init(throw_on_init)
		, m_deps(std::move(deps))
	{
	}

	void register_services(ds::app_data_storage& data) override { data.construct<game_state>(); }
	void initialize_services(ds::app_data_storage& data) override
	{
		if (m_throw_on_init) {
			throw std::runtime_error("broken game module");
		}
		data.require<game_state>().value = 42;
	}
	void shutdown_services(ds::app_data_storage& data) override { data.destruct<game_state>(); }
	std::string_view name() const override { return m_name; }
	std::vector<std::string_view> depends_on() const override { return m_deps; }

private:
	std::string m_name;
	bool m_throw_on_init = false;
	std::vector<std::string_view> m_deps;
};

prj::manifest manifest_with(std::vector<prj::module_ref> modules)
{
	prj::manifest m;
	m.name = "Sandbox";
	m.modules = std::move(modules);
	return m;
}

prj::module_catalog::factory make_module(std::string name, bool throw_on_init = false, std::vector<std::string_view> deps = {})
{
	return [name, throw_on_init, deps] { return std::make_unique<fake_module>(name, throw_on_init, deps); };
}

struct loader_fixture
{
	~loader_fixture() { loader.shutdown_all_services(data); }

	ds::app_data_storage data;
	app::modules::module_loader loader;
	prj::module_catalog catalog;
};

}

BOOST_FIXTURE_TEST_SUITE(PrjModulesTests, loader_fixture)

BOOST_AUTO_TEST_CASE(Catalog_FindsByNameAndLaterAddReplaces)
{
	BOOST_TEST(catalog.empty());
	catalog.add("game", make_module("first"));
	catalog.add("game", make_module("second"));
	BOOST_TEST(!catalog.empty());
	BOOST_TEST(catalog.find("other") == nullptr);
	BOOST_REQUIRE(catalog.find("game") != nullptr);
	BOOST_TEST((*catalog.find("game"))()->name() == "second");
}

BOOST_AUTO_TEST_CASE(Load_StartsTheManifestModuleFromTheCatalog)
{
	catalog.add("game", make_module("game"));
	const auto report = prj::load_project_modules(
		manifest_with({ { "game", prj::module_kind::runtime, false } }), catalog, true, loader, data);

	BOOST_TEST(report.loaded == std::vector<std::string>{ "game" });
	BOOST_TEST(report.missing.empty());
	BOOST_TEST(report.failed.empty());
	BOOST_TEST(data.require<game_state>().value == 42);
	BOOST_TEST((loader.state("game") == app::modules::module_state::initialized));
}

BOOST_AUTO_TEST_CASE(Load_MissingModulesAreReportedNotThrown)
{
	const auto report = prj::load_project_modules(manifest_with({
		{ "game", prj::module_kind::runtime, false },
		{ "game_editor", prj::module_kind::editor, true },
	}), catalog, true, loader, data);

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST((report.missing == std::vector<std::string>{ "game", "game_editor" }));
	BOOST_TEST(!data.has_value<game_state>());
}

BOOST_AUTO_TEST_CASE(Load_SkipsEditorModulesWhenAskedTo)
{
	catalog.add("game_editor", make_module("game_editor"));
	const auto report = prj::load_project_modules(
		manifest_with({ { "game_editor", prj::module_kind::editor, false } }), catalog, false, loader, data);

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.missing.empty());
	BOOST_TEST(!data.has_value<game_state>());
}

BOOST_AUTO_TEST_CASE(Load_AFailingModuleIsReportedAndLeavesNothingBehind)
{
	catalog.add("broken", make_module("broken", true));
	catalog.add("needs_engine", make_module("needs_engine", false, { "engine" }));
	const auto report = prj::load_project_modules(manifest_with({
		{ "broken", prj::module_kind::runtime, false },
		{ "needs_engine", prj::module_kind::runtime, false },
	}), catalog, true, loader, data);

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST((report.failed == std::vector<std::string>{ "broken", "needs_engine" }));
	BOOST_TEST(!data.has_value<game_state>());
	BOOST_TEST(data.size() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
