#include <boost/test/unit_test.hpp>
#include <snk/application/app_module_loader.hpp>
#include <snk/application/app_module_source.h>
#include <snk/common/ds/ds_store.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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

struct second_module final : app::modules::module_interface
{
	~second_module() override { g_events.emplace_back("destroy_second"); }

	void register_services(ds::app_data_storage&) override {}
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage&) override {}
	std::string_view name() const override { return "second_module"; }
};

}

SNK_DECLARE_MODULE_PREFIXED(source_test_, source_module)
SNK_DECLARE_MODULE_PREFIXED(second_test_, second_module)

namespace {

app::modules::module_entry source_module_entry()
{
	return { &source_test_module_api_version, &source_test_module_build_info, &source_test_create_module, &source_test_destroy_module };
}

app::modules::module_entry second_module_entry()
{
	return { &second_test_module_api_version, &second_test_module_build_info, &second_test_create_module, &second_test_destroy_module };
}

// This host's build info with other bits set, as a module built in another EnTT mode reports it.
snk_module_build_info g_flagged_info{};
const snk_module_build_info* flagged_build_info() { return &g_flagged_info; }

// Records open/close around a compiled-in entry.
class recording_source final : public app::modules::module_source
{
public:
	explicit recording_source(app::modules::module_entry entry, std::string open_error = {}, std::string suffix = {})
		: m_entry(entry), m_open_error(std::move(open_error)), m_suffix(std::move(suffix)) {}

	std::string open() override
	{
		g_events.emplace_back("open" + m_suffix);
		return m_open_error;
	}
	const app::modules::module_entry& entry() const override { return m_entry; }
	void close() override { g_events.emplace_back("close" + m_suffix); }
	std::string describe() const override { return "recording"; }

private:
	app::modules::module_entry m_entry;
	std::string m_open_error;
	std::string m_suffix;
};

// A module directory with a file in it and a file next to it, removed afterwards.
struct module_files {
	module_files()
		: base(std::filesystem::temp_directory_path()
			/ ("snk_module_source_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
		, dir(base / "modules")
		, inside(dir / "game.module")
		, outside(base / "outside.module")
	{
		std::filesystem::create_directories(dir);
		std::ofstream(inside) << "not a library";
		std::ofstream(outside) << "not a library";
	}
	~module_files()
	{
		std::error_code ec;
		std::filesystem::remove_all(base, ec);
	}

	std::filesystem::path base;
	std::filesystem::path dir;
	std::filesystem::path inside;
	std::filesystem::path outside;
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
	module_files files;
	const auto missing = files.dir / "no_such.module";
	auto source = std::make_unique<app::modules::native_library_source>(missing, files.dir);
	BOOST_TEST(!source->open().empty());
	BOOST_TEST(!source->is_open());

	BOOST_CHECK_THROW(loader.load(std::make_unique<app::modules::native_library_source>(missing, files.dir), data), std::logic_error);
	BOOST_TEST(!loader.state("source_module").has_value());
}

BOOST_AUTO_TEST_CASE(only_an_absolute_path_inside_the_module_directory_is_opened)
{
	module_files files;
	const std::vector<std::filesystem::path> refused{
		"game.module",                                  // a bare name: the loader's search order would pick the file
		std::filesystem::path("modules") / "game.module", // relative
		files.outside,                                  // outside the module directory
		files.dir / ".." / "outside.module",           // outside, through ".."
		files.dir,                                      // the directory itself
	};
	for (const auto& path : refused) {
		BOOST_TEST_CONTEXT(path.string()) {
			app::modules::native_library_source source(path, files.dir);
			BOOST_TEST(!source.open().empty());
			BOOST_TEST(!source.is_open());
		}
	}
	// A relative module directory is refused too, whatever the path.
	app::modules::native_library_source relative_dir(files.inside, "modules");
	BOOST_TEST(!relative_dir.open().empty());

	// The file inside passes the checks and fails only because it is not a library.
	app::modules::native_library_source inside(files.inside, files.dir);
	const std::string problem = inside.open();
	BOOST_TEST_INFO(problem);
	BOOST_TEST(problem.find("cannot open") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(the_host_build_info_carries_the_default_entt_mode)
{
	const auto flags = app::modules::make_build_info().flags;
	BOOST_TEST((flags & app::modules::build_flag::entt_standard_cpp) == 0u);
	BOOST_TEST((flags & app::modules::build_flag::entt_wide_id) == 0u);
	BOOST_TEST((flags & app::modules::build_flag::entt_custom_page) == 0u);
}

BOOST_AUTO_TEST_CASE(a_module_built_in_another_entt_mode_is_refused)
{
	for (const std::uint32_t bit : { app::modules::build_flag::entt_standard_cpp, app::modules::build_flag::entt_wide_id,
			app::modules::build_flag::entt_custom_page }) {
		BOOST_TEST_CONTEXT("bit " << bit) {
			g_events.clear();
			g_flagged_info = app::modules::make_build_info();
			g_flagged_info.flags |= bit;
			auto entry = source_module_entry();
			entry.build_info = &flagged_build_info;
			BOOST_TEST(!app::modules::check_module_entry(entry).empty());
			BOOST_CHECK_THROW(loader.load(std::make_unique<recording_source>(entry), data), std::logic_error);
			BOOST_TEST(g_events == (std::vector<std::string>{ "open", "close" }), boost::test_tools::per_element());
		}
	}
}

BOOST_AUTO_TEST_CASE(the_loader_unloads_in_reverse_load_order)
{
	{
		app::modules::module_loader scoped;
		scoped.load(std::make_unique<recording_source>(source_module_entry()), data);
		scoped.load(std::make_unique<recording_source>(second_module_entry(), std::string{}, "_second"), data);
		scoped.shutdown_all_services(data);
		g_events.clear();
	}
	BOOST_TEST(g_events == (std::vector<std::string>{ "destroy_second", "close_second", "destroy", "close" }),
		boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()
