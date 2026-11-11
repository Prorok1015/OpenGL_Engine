#include <boost/test/unit_test.hpp>
#include "test_module_env.hpp"
#include "test_temp_dir.hpp"
#include <snk/application/app_module_loader.hpp>
#include <snk/common/ds/ds_store.hpp>
#include <snk/core/core_project_modules.h>
#include <snk/desc/desc_base.hpp>
#include <snk/editor/edt_editor_extension.h>
#include <snk/game/gs_game_module.h>
#include <snk/platform/plt_process.h>
#include <snk/project/prj_project.h>
#include <snk/scene/scn_scene_layer.h>
#include <boost/json.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// core::load_project_modules (#428): the manifest's game modules, compiled in or copied out of <root>/build/bin, and
// core::project_module_host::reload (#435) with compiled-in builds. A real library is loaded and reloaded in
// tests/module_dll (shared configurations); here a file that is no library stands in for one wherever the load is
// expected to fail.

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

class first_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return "first_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer&, ds::app_data_storage&) override {}
};

class second_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return "second_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer&, ds::app_data_storage&) override {}
};

// Builds of one game module for reload: each registers a desc type of its own.
template<int Build>
class reload_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return "reload_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer& layer, ds::app_data_storage&) override
	{
		if constexpr (Build < 0) {
			throw std::runtime_error("this build fails to register");
		} else {
			layer.descs().register_component_desc<build_desc>(std::format("build{}_desc", Build));
		}
	}

private:
	struct build_desc : desc::desc_base {
		void deserialize(desc::desc_system&, const boost::json::object&) override {}
		void serialize(boost::json::object&) const override {}
	};
};

std::vector<std::string> g_editor_events;

// An editor module (kind "editor", #588) as the host sees it: only its entry and its life cycle matter here.
struct editor_probe_module final : app::modules::module_interface
{
	void register_services(ds::app_data_storage&) override { g_editor_events.push_back("editor:register"); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage&) override { g_editor_events.push_back("editor:shutdown"); }
	std::string_view name() const override { return "test_editor"; }
};

// The next build of the same editor module, one that fails to register, and a second editor module that needs the first (#664).
struct editor_probe_v2_module final : app::modules::module_interface
{
	void register_services(ds::app_data_storage&) override { g_editor_events.push_back("editor2:register"); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage&) override { g_editor_events.push_back("editor2:shutdown"); }
	std::string_view name() const override { return "test_editor"; }
};

struct editor_failing_module final : app::modules::module_interface
{
	void register_services(ds::app_data_storage&) override { throw std::runtime_error("the editor module failed to register"); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage&) override {}
	std::string_view name() const override { return "test_editor"; }
};

struct editor_dependent_module final : app::modules::module_interface
{
	void register_services(ds::app_data_storage&) override { g_editor_events.push_back("editor_b:register"); }
	void initialize_services(ds::app_data_storage&) override {}
	void shutdown_services(ds::app_data_storage&) override { g_editor_events.push_back("editor_b:shutdown"); }
	std::string_view name() const override { return "test_editor_b"; }
	std::vector<std::string_view> depends_on() const override { return { "test_editor" }; }
};

// A game module whose task continuation asks the host for a reload of its own module (from inside its callback).
std::function<void()> g_reload_from_a_callback;

class self_reload_game_module final : public gs::game_module
{
public:
	std::string_view name() const override { return "reload_game"; }
	std::vector<std::string_view> depends_on() const override { return {}; }

protected:
	void register_content(scn::scene_layer&, ds::app_data_storage& data) override
	{
		data.require<gs::module_context>()
			.run(ds::lane::compute, ds::task_desc{ .name = "game.self_reload" }, [](ds::task_context&) {})
			.then_main([](ds::task_result<void>&) { if (g_reload_from_a_callback) g_reload_from_a_callback(); });
	}
};

using reload_v1 = reload_game_module<1>;
using reload_v2 = reload_game_module<2>;
using reload_failing = reload_game_module<-1>;

}

SNK_DECLARE_EDITOR_MODULE_PREFIXED(editor_probe_, editor_probe_module)
SNK_DECLARE_EDITOR_MODULE_PREFIXED(editor_probe_v2_, editor_probe_v2_module)
SNK_DECLARE_EDITOR_MODULE_PREFIXED(editor_failing_, editor_failing_module)
SNK_DECLARE_EDITOR_MODULE_PREFIXED(editor_dependent_, editor_dependent_module)
SNK_DECLARE_MODULE_PREFIXED(self_reload_, self_reload_game_module)
SNK_DECLARE_MODULE_PREFIXED(reload_v1_, reload_v1)
SNK_DECLARE_MODULE_PREFIXED(reload_v2_, reload_v2)
SNK_DECLARE_MODULE_PREFIXED(reload_failing_, reload_failing)
SNK_DECLARE_MODULE_PREFIXED(project_first_, first_game_module)
SNK_DECLARE_MODULE_PREFIXED(project_second_, second_game_module)
SNK_DECLARE_MODULE_PREFIXED(project_probe_, probe_module)
SNK_DECLARE_MODULE_PREFIXED(project_other_, other_module)

namespace {

std::uint32_t next_major_api() { return SNK_MODULE_API_VERSION + 0x0001'0000u; }

app::modules::module_entry probe_entry()
{
	return SNK_MODULE_ENTRY(project_probe_);
}

app::modules::module_entry other_entry()
{
	return SNK_MODULE_ENTRY(project_other_);
}

app::modules::module_entry first_game_entry()
{
	return SNK_MODULE_ENTRY(project_first_);
}

app::modules::module_entry second_game_entry()
{
	return SNK_MODULE_ENTRY(project_second_);
}

app::modules::module_entry editor_probe_entry()
{
	return SNK_EDITOR_MODULE_ENTRY(editor_probe_);
}

app::modules::module_entry editor_probe_v2_entry()
{
	return SNK_EDITOR_MODULE_ENTRY(editor_probe_v2_);
}

app::modules::module_entry editor_failing_entry()
{
	return SNK_EDITOR_MODULE_ENTRY(editor_failing_);
}

app::modules::module_entry editor_dependent_entry()
{
	return SNK_EDITOR_MODULE_ENTRY(editor_dependent_);
}

std::string check_editor_entry(const app::modules::module_entry& entry)
{
	return edt::check_editor_api_version(entry.editor_api_version);
}

app::modules::module_entry self_reload_entry()
{
	return SNK_MODULE_ENTRY(self_reload_);
}

app::modules::module_entry reload_v1_entry()
{
	return SNK_MODULE_ENTRY(reload_v1_);
}

app::modules::module_entry reload_v2_entry()
{
	return SNK_MODULE_ENTRY(reload_v2_);
}

app::modules::module_entry reload_failing_entry()
{
	return SNK_MODULE_ENTRY(reload_failing_);
}

std::unique_ptr<app::modules::module_source> compiled(app::modules::module_entry entry)
{
	return std::make_unique<app::modules::static_module_source>("reload_game", entry);
}

std::function<std::unique_ptr<app::modules::module_source>()> rollback_to(app::modules::module_entry entry)
{
	return [entry] { return compiled(entry); };
}

// No process has this id on any system the tests run on (Linux pid_max is below 2^22, Windows ids are 32-bit).
constexpr std::int64_t k_dead_pid = 2147483000;
// Always running: init on Linux, System on Windows.
#if defined(_WIN32)
constexpr std::int64_t k_running_pid = 4;
#else
constexpr std::int64_t k_running_pid = 1;
#endif

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

// A project in the engine a game module loads into: the layer factory, which takes one game module at a time.
struct game_project_fixture : test_util::module_env {
	test_util::temp_dir dir{ "core_project_game_modules" };

	game_project_fixture()
	{
		fs::create_directories(dir.path() / "game");
		prj::project_context project;
		project.root = fs::canonical(dir.path() / "game");
		project.data.name = "Game";
		project.data.modules = { manifest_module("first_game"), manifest_module("second_game"), manifest_module("test_game") };
		root.construct<prj::project_context>(std::move(project));
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

BOOST_AUTO_TEST_CASE(a_required_module_that_is_refused_or_missing_has_failed_and_an_optional_one_has_not)
{
	open({ manifest_module("test_game"), manifest_module("needed_game"), manifest_module("extra_game", prj::module_kind::runtime, true) });
	auto entry = probe_entry();
	entry.api_version = &next_major_api;
	core::project_module_host host(loader, data, compiled_in(entry));
	const auto report = host.load_all();

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(host.failed_required(report) == (std::vector<std::string>{ "test_game", "needed_game" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_loaded_required_module_has_not_failed)
{
	open({ manifest_module("test_game") });
	core::project_module_host host(loader, data, compiled_in(probe_entry()));
	const auto report = host.load_all();

	BOOST_TEST(report.loaded == std::vector<std::string>{ "test_game" });
	BOOST_TEST(host.failed_required(report).empty());
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

BOOST_AUTO_TEST_CASE(editor_modules_are_not_loaded_without_the_editors_check)
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

BOOST_AUTO_TEST_CASE(a_compiled_in_module_is_current_and_a_refused_library_is_not)
{
	open({ manifest_module("test_game") });
	put_broken_library("test_game");
	{
		core::project_module_host host(loader, data, compiled_in(probe_entry()));
		host.load_all();
		BOOST_TEST(host.is_current("test_game"));
		loader.unload("test_game", data);
	}
	core::project_module_host host(loader, data, core::project_modules_options{});
	BOOST_TEST(host.load_all().refused == std::vector<std::string>{ "test_game" });
	BOOST_TEST(!host.is_current("test_game"));
	BOOST_TEST(!host.is_current("no_such_module"));
}

BOOST_AUTO_TEST_CASE(a_static_host_never_opens_the_build_output)
{
	open({ manifest_module("test_game"), manifest_module("extra_game", prj::module_kind::runtime, true) });
	put_broken_library("test_game");
	put_broken_library("extra_game");
	core::project_modules_options options;
	options.compiled_only = true;
	core::project_module_host host(loader, data, options);
	const auto report = host.load_all();

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.refused.empty());
	BOOST_TEST(report.missing_required == std::vector<std::string>{ "test_game" });
	BOOST_TEST(report.missing_optional == std::vector<std::string>{ "extra_game" });
	BOOST_TEST(!fs::exists(root / ".cache" / "modules"));
	BOOST_TEST(!host.build_output("test_game").has_value());
	BOOST_TEST((host.reload("test_game").status == core::reload_status::refused));
}

BOOST_AUTO_TEST_CASE(a_static_host_still_loads_its_compiled_in_modules)
{
	open({ manifest_module("test_game") });
	put_broken_library("test_game");
	auto options = compiled_in(probe_entry());
	options.compiled_only = true;
	const auto report = load(options);

	BOOST_TEST(report.loaded == std::vector<std::string>{ "test_game" });
	BOOST_TEST(report.refused.empty());
	BOOST_TEST(data.has_value<probe_service>());
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

BOOST_AUTO_TEST_CASE(copies_of_ended_processes_are_removed_and_only_those)
{
	open({ manifest_module("test_game") });
	put_broken_library("test_game");
	const fs::path cache = root / ".cache" / "modules";
	dir.write(cache / ("test_game.7" + copy_extension()), "a copy of the layout before #435");
	dir.write(cache / std::format("{}-0", k_dead_pid) / library_file("test_game"), "an editor that ended");
	dir.write(cache / std::format("{}-3", k_running_pid) / library_file("test_game"), "an editor that runs");
	dir.write(cache / std::format("{}-9", platform::current_process_id()) / library_file("test_game"), "this process, unused");
	dir.write(cache / "notes" / "keep.txt", "no copy");
	load();

	auto left = files_in(cache);
	std::ranges::sort(left);
	BOOST_TEST(left == (std::vector<std::string>{ std::format("{}-3", k_running_pid), "notes" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_static_host_leaves_the_copies_in_the_project_alone)
{
	open({ manifest_module("test_game") });
	const fs::path cache = root / ".cache" / "modules";
	dir.write(cache / std::format("{}-0", k_dead_pid) / library_file("test_game"), "an editor that ended");
	core::project_modules_options options;
	options.compiled_only = true;
	load(options);

	BOOST_TEST(files_in(cache) == (std::vector<std::string>{ std::format("{}-0", k_dead_pid) }), boost::test_tools::per_element());
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

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(a_library_without_the_lib_prefix_is_found_too)
{
	open({ manifest_module("test_game") });
	dir.write(fs::path{ "game" } / "build" / "bin" / "test_game.so", "not a library");
	const auto report = load();

	BOOST_TEST(report.missing_required.empty());
	BOOST_TEST(report.refused == std::vector<std::string>{ "test_game" });
}
#endif

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(core_project_game_modules_tests, game_project_fixture)

BOOST_AUTO_TEST_CASE(a_second_game_module_is_refused_and_the_rest_still_loads)
{
	core::project_modules_options options;
	options.compiled_in.emplace("first_game", first_game_entry());
	options.compiled_in.emplace("second_game", second_game_entry());
	options.compiled_in.emplace("test_game", probe_entry());
	const auto report = core::load_project_modules(loader, root, options);

	BOOST_TEST(report.loaded == (std::vector<std::string>{ "first_game", "test_game" }));
	BOOST_TEST(report.refused == std::vector<std::string>{ "second_game" });
	BOOST_TEST((loader.state("first_game") == app::modules::module_state::initialized));
	BOOST_TEST(!loader.state("second_game").has_value());
	BOOST_TEST(contexts().game_layer() != nullptr);
}

BOOST_AUTO_TEST_SUITE_END()

// The engine side with the game layer factory, and a host over it: reload() with compiled-in builds.
struct reload_fixture : test_util::module_env {
	core::project_module_host host{ loader, root, core::project_modules_options{} };
	std::vector<std::string> calls;
	core::reload_hooks hooks;

	reload_fixture()
	{
		hooks.before_swap = [this] { calls.push_back("before"); };
		hooks.after_swap = [this](const core::reload_report& report) { calls.push_back(std::string{ "after:" } + std::string{ core::to_string(report.status) }); };
		loader.load(compiled(reload_v1_entry()), root);
	}

	bool registered(std::string_view type)
	{
		auto* layer = contexts().game_layer();
		return layer && layer->descs().is_registered(type);
	}
};

BOOST_FIXTURE_TEST_SUITE(core_project_module_reload_tests, reload_fixture)

BOOST_AUTO_TEST_CASE(a_new_build_takes_the_place_of_the_running_one)
{
	const auto report = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST(report.message.empty());
	BOOST_TEST(registered("build2_desc"));
	BOOST_TEST(!registered("build1_desc"));
	BOOST_TEST((calls == std::vector<std::string>{ "before", "after:reloaded" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_build_of_another_api_is_refused_and_the_running_one_is_not_touched)
{
	auto entry = reload_v2_entry();
	entry.api_version = &next_major_api;
	auto* layer = contexts().game_layer();
	const auto report = host.reload("reload_game", compiled(entry), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("api") != std::string::npos);
	BOOST_TEST(calls.empty());
	BOOST_TEST(contexts().game_layer() == layer);
	BOOST_TEST(registered("build1_desc"));
}

BOOST_AUTO_TEST_CASE(a_build_of_another_module_is_refused)
{
	const auto report = host.reload("reload_game", std::make_unique<app::modules::static_module_source>("x", other_entry()),
		rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("other_game") != std::string::npos);
	BOOST_TEST(calls.empty());
	BOOST_TEST(registered("build1_desc"));
}

BOOST_AUTO_TEST_CASE(a_build_that_fails_to_register_rolls_back_to_the_previous_one)
{
	const auto report = host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::rolled_back));
	BOOST_TEST(report.message.find("fails to register") != std::string::npos);
	BOOST_TEST((loader.state("reload_game") == app::modules::module_state::initialized));
	BOOST_TEST(registered("build1_desc"));
	BOOST_TEST((calls == std::vector<std::string>{ "before", "after:rolled_back" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(when_the_previous_build_fails_too_no_module_runs)
{
	const auto report = host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_failing_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::lost));
	BOOST_TEST((loader.state("reload_game") == app::modules::module_state::unloaded));
	BOOST_TEST(contexts().game_layer() != nullptr);
	BOOST_TEST(!registered("build1_desc"));
	BOOST_TEST((calls == std::vector<std::string>{ "before", "after:lost" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_reload_asked_for_from_inside_a_reload_is_refused)
{
	std::optional<core::reload_report> nested;
	hooks.before_swap = [&] {
		nested = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()));
	};
	const auto report = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_REQUIRE(nested.has_value());
	BOOST_TEST((nested->status == core::reload_status::refused));
	BOOST_TEST(nested->message.find("already running") != std::string::npos);
	BOOST_TEST(registered("build2_desc"));
}

BOOST_AUTO_TEST_CASE(a_failed_build_with_nothing_running_is_refused_not_lost)
{
	loader.unload("reload_game", root);
	const auto report = host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("fails to register") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(without_a_build_in_the_project_nothing_is_reloaded)
{
	const auto report = host.reload("reload_game", hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(calls.empty());
	BOOST_TEST(registered("build1_desc"));
}

// #601 (HOT_RELOAD_PLAY.md 3.5): the build that ran before the last reload comes back, through the same swap, once.
BOOST_AUTO_TEST_CASE(a_revert_brings_the_previous_build_back_once)
{
	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks).status
		== core::reload_status::reloaded));
	calls.clear();

	const auto report = host.revert("reload_game", hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST(registered("build1_desc"));
	BOOST_TEST(!registered("build2_desc"));
	BOOST_TEST((calls == std::vector<std::string>{ "before", "after:reloaded" }), boost::test_tools::per_element());

	calls.clear();
	const auto again = host.revert("reload_game", hooks);
	BOOST_TEST((again.status == core::reload_status::refused));
	BOOST_TEST(again.message.find("no reload") != std::string::npos);
	BOOST_TEST(calls.empty());
	BOOST_TEST(registered("build1_desc"));
}

BOOST_AUTO_TEST_CASE(without_a_reload_or_after_a_rollback_there_is_nothing_to_revert)
{
	BOOST_TEST((host.revert("reload_game", hooks).status == core::reload_status::refused));
	BOOST_TEST(calls.empty());

	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_v1_entry()), hooks).status
		== core::reload_status::rolled_back));
	calls.clear();
	BOOST_TEST((host.revert("reload_game", hooks).status == core::reload_status::refused));
	BOOST_TEST(calls.empty());
	BOOST_TEST(registered("build1_desc"));

	// A point an earlier reload left goes with a later one that swapped and rolled back: no empty or stale point remains.
	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks).status
		== core::reload_status::reloaded));
	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_v2_entry()), hooks).status
		== core::reload_status::rolled_back));
	calls.clear();
	const auto stale = host.revert("reload_game", hooks);
	BOOST_TEST((stale.status == core::reload_status::refused));
	BOOST_TEST(stale.message.find("no reload of it to take back") != std::string::npos);
	BOOST_TEST(calls.empty());
	BOOST_TEST(registered("build2_desc"));
}

BOOST_AUTO_TEST_CASE(a_revert_after_the_module_was_unloaded_is_refused)
{
	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks).status
		== core::reload_status::reloaded));
	loader.unload("reload_game", root);
	calls.clear();

	const auto report = host.revert("reload_game", hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("not loaded") != std::string::npos);
	BOOST_TEST(calls.empty());
	BOOST_TEST((loader.state("reload_game") != app::modules::module_state::initialized));
}

BOOST_AUTO_TEST_CASE(a_revert_asked_for_from_inside_a_reload_is_refused)
{
	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks).status
		== core::reload_status::reloaded));
	std::optional<core::reload_report> nested;
	hooks.before_swap = [&] { nested = host.revert("reload_game"); };

	const auto report = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_REQUIRE(nested.has_value());
	BOOST_TEST((nested->status == core::reload_status::refused));
	BOOST_TEST(nested->message.find("already running") != std::string::npos);
}

// A previous build that fails to load again is not followed by another try: no module runs.
BOOST_AUTO_TEST_CASE(a_revert_whose_previous_build_fails_leaves_no_module)
{
	BOOST_REQUIRE((host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_failing_entry()), hooks).status
		== core::reload_status::reloaded));
	calls.clear();

	const auto report = host.revert("reload_game", hooks);

	BOOST_TEST((report.status == core::reload_status::lost));
	BOOST_TEST((loader.state("reload_game") == app::modules::module_state::unloaded));
	BOOST_TEST((calls == std::vector<std::string>{ "before", "after:lost" }), boost::test_tools::per_element());
	BOOST_TEST((host.revert("reload_game", hooks).status == core::reload_status::refused));
}

BOOST_AUTO_TEST_SUITE_END()

// Editor modules (#588): loaded after the game modules by load_editor_modules, unloaded before a game reload swaps and
// loaded again after it.
struct editor_module_fixture : test_util::module_env {
	test_util::temp_dir dir{ "core_project_editor_modules" };
	core::project_module_host host;
	std::string refusal;
	std::vector<std::string> calls;
	core::reload_hooks hooks;

	static core::project_modules_options options_for(std::string* refusal)
	{
		core::project_modules_options options;
		options.with_editor_modules = true;
		options.check_editor_module = &check_editor_entry;
		options.editor_unload_refusal = [refusal] { return *refusal; };
		options.compiled_in.emplace("test_editor", editor_probe_entry());
		return options;
	}

	editor_module_fixture()
		: host(loader, root, options_for(&refusal))
	{
		g_editor_events.clear();
		fs::create_directories(dir.path() / "game");
		prj::project_context project;
		project.root = fs::canonical(dir.path() / "game");
		project.data.name = "Game";
		project.data.modules = { manifest_module("test_editor", prj::module_kind::editor) };
		root.construct<prj::project_context>(std::move(project));
		hooks.before_swap = [] { g_editor_events.push_back("before"); };
		hooks.after_swap = [](const core::reload_report& report) { g_editor_events.push_back(std::string{ "after:" } + std::string{ core::to_string(report.status) }); };
		loader.load(compiled(reload_v1_entry()), root);
	}

	bool editor_runs() const { return loader.state("test_editor") == app::modules::module_state::initialized; }
};

BOOST_FIXTURE_TEST_SUITE(core_project_editor_module_tests, editor_module_fixture)

BOOST_AUTO_TEST_CASE(an_editor_module_is_loaded_after_the_game_and_unloaded_on_request)
{
	const auto report = host.load_editor_modules();
	BOOST_TEST(report.loaded == std::vector<std::string>{ "test_editor" });
	BOOST_TEST(editor_runs());
	BOOST_TEST(host.has_editor_modules());

	BOOST_TEST(host.unload_editor_modules().empty());
	BOOST_TEST(!editor_runs());
	BOOST_TEST(!host.has_editor_modules());
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "editor:register", "editor:shutdown" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(an_editor_module_of_another_editor_api_version_is_refused_and_the_game_goes_on)
{
	core::project_modules_options options = options_for(&refusal);
	options.check_editor_module = [](const app::modules::module_entry&) { return std::string{ "editor api major version 9 does not match host major 1" }; };
	core::project_module_host strict(loader, root, options);
	const auto report = strict.load_editor_modules();

	BOOST_TEST(report.loaded.empty());
	BOOST_TEST(report.refused == std::vector<std::string>{ "test_editor" });
	BOOST_TEST(!loader.state("test_editor").has_value());
	BOOST_TEST((loader.state("reload_game") == app::modules::module_state::initialized));
	BOOST_TEST(g_editor_events.empty());
}

BOOST_AUTO_TEST_CASE(a_module_without_the_editor_api_entry_is_refused_by_the_check)
{
	auto entry = editor_probe_entry();
	entry.editor_api_version = nullptr;
	core::project_modules_options options = options_for(&refusal);
	options.compiled_in["test_editor"] = entry;
	core::project_module_host host_without(loader, root, options);
	const auto report = host_without.load_editor_modules();

	BOOST_TEST(report.refused == std::vector<std::string>{ "test_editor" });
	BOOST_TEST(!loader.state("test_editor").has_value());
}

BOOST_AUTO_TEST_CASE(a_game_reload_unloads_the_editor_modules_first_and_loads_them_again_after_the_swap)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST(editor_runs());
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "editor:shutdown", "before", "editor:register", "after:reloaded" }),
		boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(a_failed_game_reload_brings_the_editor_modules_back_too)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::rolled_back));
	BOOST_TEST(editor_runs());
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "editor:shutdown", "before", "editor:register", "after:rolled_back" }),
		boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(an_unload_the_editor_refuses_stops_the_reload_before_anything_is_touched)
{
	host.load_editor_modules();
	g_editor_events.clear();
	refusal = "called from inside one of a game_editor module's callbacks";
	const auto report = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(reload_v1_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("callbacks") != std::string::npos);
	BOOST_TEST(g_editor_events.empty());
	BOOST_TEST(editor_runs());
	BOOST_TEST(host.has_editor_modules());
	BOOST_TEST(!host.unload_editor_modules().empty());
}

BOOST_AUTO_TEST_CASE(a_reload_asked_for_from_inside_a_game_callback_is_refused_before_the_editor_modules_are_touched)
{
	root.attach(test_util::borrowed(tasks));
	loader.unload("reload_game", root);
	loader.load(compiled(self_reload_entry()), root);
	host.load_editor_modules();
	g_editor_events.clear();

	core::reload_report report;
	g_reload_from_a_callback = [&] {
		report = host.reload("reload_game", compiled(reload_v2_entry()), rollback_to(self_reload_entry()), hooks);
	};
	tasks.run_one(ds::lane::compute);
	tasks.drain_main();
	g_reload_from_a_callback = nullptr;

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("callbacks") != std::string::npos);
	BOOST_TEST(g_editor_events.empty());
	BOOST_TEST(editor_runs());
}

BOOST_AUTO_TEST_CASE(an_editor_module_is_not_taken_back_on_its_own)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.revert("test_editor", hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("editor module") != std::string::npos);
	BOOST_TEST(g_editor_events.empty());
	BOOST_TEST(editor_runs());
}

BOOST_AUTO_TEST_CASE(an_editor_module_is_reloaded_on_its_own_and_the_game_module_goes_on)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_TEST(report.module == "test_editor");
	BOOST_TEST(editor_runs());
	BOOST_TEST(host.has_editor_modules());
	BOOST_TEST((loader.state("reload_game") == app::modules::module_state::initialized));
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "before", "editor:shutdown", "editor2:register", "after:reloaded" }),
		boost::test_tools::per_element());
	BOOST_TEST(host.unload_editor_modules().empty());
	BOOST_TEST((g_editor_events.back() == "editor2:shutdown"));
}

BOOST_AUTO_TEST_CASE(a_new_editor_build_that_fails_to_register_is_replaced_by_the_previous_one)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.reload("test_editor", compiled(editor_failing_entry()), rollback_to(editor_probe_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::rolled_back));
	BOOST_TEST(editor_runs());
	BOOST_TEST(host.has_editor_modules());
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "before", "editor:shutdown", "editor:register", "after:rolled_back" }),
		boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(an_editor_module_whose_previous_build_fails_too_stays_unloaded_and_loads_with_the_next_good_build)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.reload("test_editor", compiled(editor_failing_entry()), rollback_to(editor_failing_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::lost));
	BOOST_TEST(!editor_runs());
	BOOST_TEST(!host.has_editor_modules());
	BOOST_TEST((loader.state("reload_game") == app::modules::module_state::initialized));
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "before", "editor:shutdown", "after:lost" }), boost::test_tools::per_element());
	BOOST_TEST(host.unload_editor_modules().empty());

	const auto again = host.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()), hooks);
	BOOST_TEST((again.status == core::reload_status::reloaded));
	BOOST_TEST(editor_runs());
	BOOST_TEST(host.has_editor_modules());
}

BOOST_AUTO_TEST_CASE(an_editor_reload_the_editor_refuses_touches_nothing)
{
	host.load_editor_modules();
	g_editor_events.clear();
	refusal = "called from inside one of a game_editor module's callbacks";
	const auto report = host.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("callbacks") != std::string::npos);
	BOOST_TEST(g_editor_events.empty());
	BOOST_TEST(editor_runs());
	BOOST_TEST(host.has_editor_modules());

	refusal.clear();
	BOOST_TEST((host.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()), hooks).status
		== core::reload_status::reloaded));
}

BOOST_AUTO_TEST_CASE(an_editor_module_another_one_depends_on_is_not_reloaded_and_nothing_is_touched)
{
	core::project_modules_options options = options_for(&refusal);
	options.compiled_in.emplace("test_editor_b", editor_dependent_entry());
	root.require<prj::project_context>().data.modules.push_back(manifest_module("test_editor_b", prj::module_kind::editor));
	core::project_module_host two(loader, root, options);
	BOOST_REQUIRE((two.load_editor_modules().loaded == std::vector<std::string>{ "test_editor", "test_editor_b" }));
	g_editor_events.clear();

	const auto report = two.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::refused));
	BOOST_TEST(report.message.find("test_editor_b") != std::string::npos);
	BOOST_TEST(report.message.find("depends on it") != std::string::npos);
	BOOST_TEST(g_editor_events.empty());
	BOOST_TEST((loader.state("test_editor") == app::modules::module_state::initialized));
	BOOST_TEST((loader.state("test_editor_b") == app::modules::module_state::initialized));

	BOOST_TEST((two.reload("test_editor_b", compiled(editor_dependent_entry()), rollback_to(editor_dependent_entry()), hooks).status
		== core::reload_status::reloaded));
}

BOOST_AUTO_TEST_CASE(a_second_reload_of_an_editor_module_is_refused_while_one_runs_and_a_module_that_is_not_an_editor_one_is_not_taken)
{
	host.load_editor_modules();
	std::optional<core::reload_report> nested;
	hooks.before_swap = [&] {
		nested = host.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()));
	};
	const auto report = host.reload("test_editor", compiled(editor_probe_v2_entry()), rollback_to(editor_probe_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::reloaded));
	BOOST_REQUIRE(nested.has_value());
	BOOST_TEST((nested->status == core::reload_status::refused));
	BOOST_TEST(nested->message.find("already running") != std::string::npos);
	BOOST_TEST((host.reload_editor_module("reload_game").status == core::reload_status::refused));
}

BOOST_AUTO_TEST_CASE(when_no_game_module_runs_after_a_reload_the_editor_modules_stay_unloaded)
{
	host.load_editor_modules();
	g_editor_events.clear();
	const auto report = host.reload("reload_game", compiled(reload_failing_entry()), rollback_to(reload_failing_entry()), hooks);

	BOOST_TEST((report.status == core::reload_status::lost));
	BOOST_TEST(!editor_runs());
	BOOST_TEST((g_editor_events == std::vector<std::string>{ "editor:shutdown", "before", "after:lost" }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_SUITE_END()
