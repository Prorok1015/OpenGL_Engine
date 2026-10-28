#include <boost/test/unit_test.hpp>
#include "test_temp_dir.hpp"
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/core/core_project_modules.h>
#include <snk/project/prj_project.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// core::load_project_modules (#428): the manifest's game modules, compiled in or copied out of <root>/build/bin. A
// real library is loaded in tests/module_dll (shared configurations); here a file that is no library stands in for
// one wherever the load is expected to fail.

namespace fs = std::filesystem;

namespace {

int g_alive = 0;

struct probe_service { int v = 1; };

struct probe_module final : app::modules::module_interface
{
	probe_module() { ++g_alive; }
	~probe_module() override { --g_alive; }

	void register_services(ds::app_data_storage& data) override { data.construct<probe_service>(); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage& data) override { data.destruct<probe_service>(); }
	std::string_view name() const override { return "test_game"; }
};

struct other_module final : app::modules::module_interface
{
	other_module() { ++g_alive; }
	~other_module() override { --g_alive; }

	void register_services(ds::app_data_storage&) override {}
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage&) override {}
	std::string_view name() const override { return "other_game"; }
};

}

SNK_DECLARE_MODULE_PREFIXED(project_probe_, probe_module)
SNK_DECLARE_MODULE_PREFIXED(project_other_, other_module)

namespace {

std::uint32_t next_major_api() { return SNK_MODULE_API_VERSION + 0x0001'0000u; }

app::modules::module_entry probe_entry()
{
	return { &project_probe_module_api_version, &project_probe_module_build_info, &project_probe_create_module, &project_probe_destroy_module };
}

app::modules::module_entry other_entry()
{
	return { &project_other_module_api_version, &project_other_module_build_info, &project_other_create_module, &project_other_destroy_module };
}

std::string library_file(const std::string& name)
{
#if defined(_WIN32)
	return name + ".dll";
#else
	return "lib" + name + ".so";
#endif
}

std::string copy_extension()
{
#if defined(_WIN32)
	return ".dll";
#else
	return ".so";
#endif
}

prj::module_ref manifest_module(std::string name, prj::module_kind kind = prj::module_kind::runtime, bool optional = false)
{
	prj::module_ref ref;
	ref.name = std::move(name);
	ref.kind = kind;
	ref.optional = optional;
	return ref;
}

std::vector<std::string> files_in(const fs::path& dir)
{
	std::vector<std::string> names;
	std::error_code ec;
	for (const auto& item : fs::directory_iterator(dir, ec)) {
		names.push_back(item.path().filename().string());
	}
	return names;
}

struct project_fixture {
	test_util::temp_dir dir{ "core_project_modules" };
	fs::path root;
	ds::app_data_storage data;
	app::modules::module_loader loader;

	project_fixture()
	{
		fs::create_directories(dir.path() / "game");
		root = fs::canonical(dir.path() / "game");
	}

	~project_fixture()
	{
		loader.shutdown_all_services(data);
	}

	void open(std::vector<prj::module_ref> modules)
	{
		prj::project_context project;
		project.root = root;
		project.manifest_file = root / "project.snkproj";
		project.data.name = "Game";
		project.data.modules = std::move(modules);
		data.construct<prj::project_context>(std::move(project));
	}

	// Not a library: opening it fails on every platform.
	fs::path put_broken_library(const std::string& name)
	{
		return dir.write(fs::path{ "game" } / "build" / "bin" / library_file(name), "not a library");
	}

	core::project_modules_report load(core::project_modules_options options = {})
	{
		return core::load_project_modules(loader, data, options);
	}

	static core::project_modules_options compiled_in(app::modules::module_entry entry)
	{
		core::project_modules_options options;
		options.compiled_in.emplace("test_game", entry);
		return options;
	}
};

bool symlinks_refused(std::error_code ec)
{
	if (ec) {
		// Windows without the symlink privilege: nothing to check.
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
	}
	return static_cast<bool>(ec);
}

}

BOOST_FIXTURE_TEST_SUITE(core_project_modules_tests, project_fixture)

BOOST_AUTO_TEST_CASE(without_a_project_nothing_is_loaded)
{
	const auto report = load(compiled_in(probe_entry()));
	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(!loader.state("test_game").has_value());
}

BOOST_AUTO_TEST_CASE(a_compiled_in_module_named_by_the_manifest_is_loaded)
{
	open({ manifest_module("test_game") });
	const auto report = load(compiled_in(probe_entry()));

	BOOST_TEST(report.loaded == std::vector<std::string>{ "test_game" });
	BOOST_TEST(report.refused.empty());
	BOOST_TEST((loader.state("test_game") == app::modules::module_state::initialized));
	BOOST_TEST(data.has_value<probe_service>());
}

BOOST_AUTO_TEST_CASE(a_module_of_another_api_version_is_refused_and_the_host_goes_on)
{
	open({ manifest_module("test_game") });
	auto entry = probe_entry();
	entry.api_version = &next_major_api;
	const auto report = load(compiled_in(entry));

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(!loader.state("test_game").has_value());
	BOOST_TEST(!data.has_value<probe_service>());
}

BOOST_AUTO_TEST_CASE(a_module_that_is_another_one_is_refused_before_it_registers)
{
	open({ manifest_module("test_game") });
	const auto report = load(compiled_in(other_entry()));

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(!loader.state("other_game").has_value());
	BOOST_TEST(g_alive == 0);
}

BOOST_AUTO_TEST_CASE(missing_modules_are_reported_and_the_host_goes_on)
{
	open({ manifest_module("test_game"), manifest_module("extra_game", prj::module_kind::runtime, true) });
	const auto report = load();

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.refused.empty());
	BOOST_TEST(report.missing_required == std::vector<std::string>{ "test_game" });
	BOOST_TEST(report.missing_optional == std::vector<std::string>{ "extra_game" });
}

BOOST_AUTO_TEST_CASE(editor_modules_are_not_loaded_yet)
{
	open({ manifest_module("test_game", prj::module_kind::editor) });
	auto options = compiled_in(probe_entry());
	options.with_editor_modules = true;
	const auto report = load(options);

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.refused.empty());
	BOOST_TEST(report.missing_required.empty());
	BOOST_TEST(!loader.state("test_game").has_value());
}

BOOST_AUTO_TEST_CASE(a_compiled_in_module_wins_over_the_build_output)
{
	open({ manifest_module("test_game") });
	put_broken_library("test_game");
	const auto report = load(compiled_in(probe_entry()));

	BOOST_TEST(report.loaded == std::vector<std::string>{ "test_game" });
	BOOST_TEST(files_in(root / ".cache" / "modules").empty());
}

BOOST_AUTO_TEST_CASE(a_library_that_cannot_be_opened_is_refused_and_its_copy_removed)
{
	open({ manifest_module("test_game") });
	const fs::path library = put_broken_library("test_game");
	const auto report = load();

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(report.missing_required.empty());
	BOOST_TEST(fs::is_directory(root / ".cache" / "modules"));
	BOOST_TEST(files_in(root / ".cache" / "modules").empty());
	BOOST_TEST(fs::exists(library));
}

BOOST_AUTO_TEST_CASE(copies_left_by_earlier_runs_are_removed_and_only_those)
{
	open({ manifest_module("test_game") });
	put_broken_library("test_game");
	const fs::path cache = root / ".cache" / "modules";
	dir.write(cache / ("test_game.7" + copy_extension()), "old");
	dir.write(cache / "test_game.7.pdb", "old");
	dir.write(cache / ("other_game.3" + copy_extension()), "other");
	dir.write(cache / ("test_game_x.1" + copy_extension()), "other");
	dir.write(cache / "test_game.notes", "other");
	load();

	auto left = files_in(cache);
	std::ranges::sort(left);
	BOOST_TEST(left == (std::vector<std::string>{ "other_game.3" + copy_extension(), "test_game.notes", "test_game_x.1" + copy_extension() }));
}

BOOST_AUTO_TEST_CASE(a_build_output_linked_out_of_the_project_is_refused)
{
	open({ manifest_module("test_game") });
	dir.write(fs::path{ "outside" } / library_file("test_game"), "not a library");
	fs::create_directories(root / "build");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", root / "build" / "bin", ec);
	if (symlinks_refused(ec)) {
		return;
	}
	const auto report = load();

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(!fs::exists(root / ".cache" / "modules"));
}

BOOST_AUTO_TEST_CASE(a_library_linked_out_of_the_build_output_is_refused)
{
	open({ manifest_module("test_game") });
	const fs::path outside = dir.write(fs::path{ "outside" } / library_file("test_game"), "not a library");
	fs::create_directories(root / "build" / "bin");
	std::error_code ec;
	fs::create_symlink(outside, root / "build" / "bin" / library_file("test_game"), ec);
	if (symlinks_refused(ec)) {
		return;
	}
	const auto report = load();

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(!fs::exists(root / ".cache" / "modules"));
}

BOOST_AUTO_TEST_CASE(a_cache_linked_out_of_the_project_gets_no_copy)
{
	open({ manifest_module("test_game") });
	put_broken_library("test_game");
	fs::create_directories(dir.path() / "outside");
	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", root / ".cache", ec);
	if (symlinks_refused(ec)) {
		return;
	}
	const auto report = load();

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(!fs::exists(dir.path() / "outside" / "modules"));
}

BOOST_AUTO_TEST_SUITE_END()
