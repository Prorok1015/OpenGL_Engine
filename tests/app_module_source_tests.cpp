#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_loader.hpp>
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_store.hpp>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// module_loader with a module_source (#220 PR-7, MODULE_LAYER.md §6): the source is opened before the module is
// created and closed only after the module is destroyed, also when the load is refused. A real shared library is
// loaded in tests/module_dll (shared configurations).

namespace {

std::vector<std::string> g_events;

struct source_service { int v = 3; };

struct source_module final : app::modules::module_interface
{
	~source_module() override { g_events.emplace_back("destroy"); }

	void register_services(ds::app_data_storage& data) override { data.construct<source_service>(); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage& data) override { data.destruct<source_service>(); }
	std::string_view name() const override { return "source_module"; }
};

}

SNK_DECLARE_MODULE_PREFIXED(source_test_, source_module)

namespace {

app::modules::module_entry source_module_entry()
{
	return { &source_test_module_api_version, &source_test_module_build_info, &source_test_create_module, &source_test_destroy_module };
}

// Records open/close around a compiled-in entry.
class recording_source final : public app::modules::module_source
{
public:
	explicit recording_source(app::modules::module_entry entry, std::string open_error = {})
		: m_entry(entry), m_open_error(std::move(open_error)) {}

	std::string open() override
	{
		g_events.emplace_back("open");
		return m_open_error;
	}
	const app::modules::module_entry& entry() const override { return m_entry; }
	void close() override { g_events.emplace_back("close"); }
	std::string describe() const override { return "recording"; }

private:
	app::modules::module_entry m_entry;
	std::string m_open_error;
};

struct source_fixture {
	source_fixture() { g_events.clear(); }

	ds::app_data_storage data;
	app::modules::module_loader loader;
};

}

BOOST_FIXTURE_TEST_SUITE(app_module_source_tests, source_fixture)

BOOST_AUTO_TEST_CASE(a_static_source_loads_its_module)
{
	loader.load(std::make_unique<app::modules::static_module_source>("source_module", source_module_entry()), data);
	BOOST_TEST((loader.state("source_module") == app::modules::module_state::initialized));
	BOOST_TEST(data.has_value<source_service>());

	loader.unload("source_module", data);
	BOOST_TEST(!data.has_value<source_service>());
	BOOST_TEST((loader.state("source_module") == app::modules::module_state::unloaded));
}

BOOST_AUTO_TEST_CASE(the_source_is_closed_after_the_module_is_destroyed_on_unload)
{
	loader.load(std::make_unique<recording_source>(source_module_entry()), data);
	BOOST_TEST(g_events == std::vector<std::string>{ "open" }, boost::test_tools::per_element());

	loader.unload("source_module", data);
	BOOST_TEST(g_events == (std::vector<std::string>{ "open", "destroy", "close" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(the_source_is_closed_after_the_module_when_the_loader_goes_away)
{
	{
		app::modules::module_loader scoped;
		scoped.load(std::make_unique<recording_source>(source_module_entry()), data);
		scoped.shutdown_all_services(data);
	}
	BOOST_TEST(g_events == (std::vector<std::string>{ "open", "destroy", "close" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_source_that_cannot_be_opened_is_refused)
{
	BOOST_CHECK_THROW(loader.load(std::make_unique<recording_source>(source_module_entry(), "no such file"), data), std::logic_error);
	BOOST_TEST(!loader.state("source_module").has_value());
	BOOST_TEST(g_events == (std::vector<std::string>{ "open", "close" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_refused_entry_closes_the_source_without_creating_the_module)
{
	auto entry = source_module_entry();
	entry.create = nullptr;
	BOOST_CHECK_THROW(loader.load(std::make_unique<recording_source>(entry), data), std::logic_error);
	BOOST_TEST(g_events == (std::vector<std::string>{ "open", "close" }), boost::test_tools::per_element());
	BOOST_TEST(data.size() == 0u);
}

BOOST_AUTO_TEST_CASE(a_missing_library_is_refused)
{
	const auto missing = std::filesystem::temp_directory_path() / "snk_no_such_module.module";
	auto source = std::make_unique<app::modules::native_library_source>(missing);
	BOOST_TEST(!source->open().empty());
	BOOST_TEST(!source->is_open());

	BOOST_CHECK_THROW(loader.load(std::make_unique<app::modules::native_library_source>(missing), data), std::logic_error);
	BOOST_TEST(!loader.state("source_module").has_value());
}

BOOST_AUTO_TEST_CASE(the_host_build_info_carries_the_default_entt_mode)
{
	const auto flags = app::modules::make_build_info().flags;
	BOOST_TEST((flags & app::modules::build_flag::entt_standard_cpp) == 0u);
	BOOST_TEST((flags & app::modules::build_flag::entt_wide_id) == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
