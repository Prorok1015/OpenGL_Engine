#include <boost/test/unit_test.hpp>
#include "edt_git_panel.h"
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <snk/resource/res_file_change.h>
#include <snk/resource/res_mount_table.h>
#include <snk/resource/res_system.h>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// The Git panel follows asset edits in res/ as it follows code/ (#515): visible, it reads again after the quiet time;
// hidden, it only marks itself stale and asks git for nothing.
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;
using namespace std::chrono_literals;

struct git_panel_fixture
{
	test_util::temp_dir dir{ "edt_git_panel" };
	fs::path project = dir.path() / "project";
	fs::path content = project / "res";
	fs::path cache = project / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };
	std::unique_ptr<res::resource_system> sys;
	edt::editor_events events;
	edt::dialog_service dialogs;
	std::vector<std::string> calls;
	std::uint64_t next_job = 1;
	edt::git_panel_model::clock::time_point now{};
	std::unique_ptr<edt::git_panel> panel;
	std::vector<res::file_change> seen;
	int probe = 0;

	git_panel_fixture()
	{
		fs::create_directories(content / "levels");
		fs::create_directories(cache / "memory");
		dir.write("project/res/levels/main.desc", "level");
		sys = std::make_unique<res::resource_system>(tasks,
			res::resource_system::project_mount_table({}, { content }, cache, cache / "user_profile"));
		panel = std::make_unique<edt::git_panel>(events, ports(), dialogs, *sys);
		// Delivered with the panel's own subscription: once the probe has a batch, the panel has it too.
		sys->subscribe_changes(res::tag::default_protocol(), &probe, [this](std::span<const res::file_change> batch) {
			seen.insert(seen.end(), batch.begin(), batch.end());
		});
	}

	~git_panel_fixture()
	{
		sys->unsubscribe_changes(&probe);
		panel.reset();
	}

	edt::git_panel_model::ports ports()
	{
		edt::git_panel_model::ports out;
		out.invoke = [this](std::string_view id, json::value) {
			calls.emplace_back(id);
			if (id == "vcs.info") return cmd::result::success(json::object{ { "available", true } });
			return cmd::result::success(json::object{ { "job", next_job++ } });
		};
		out.cancel = [](std::uint64_t) { return true; };
		out.now = [this] { return now; };
		return out;
	}

	std::size_t count(std::string_view id) const
	{
		return static_cast<std::size_t>(std::ranges::count_if(calls, [id](const std::string& call) { return call == id; }));
	}

	// Pumps the main-thread queue (where the watcher's batches arrive) and lets the panel's clock run.
	template<class Predicate>
	bool pump_until(Predicate predicate, std::chrono::milliseconds limit = 8000ms)
	{
		const auto deadline = std::chrono::steady_clock::now() + limit;
		while (!predicate()) {
			test_util::pump(tasks);
			now += 2s;
			panel->model().tick();
			if (std::chrono::steady_clock::now() > deadline) {
				return predicate();
			}
			std::this_thread::sleep_for(20ms);
		}
		return true;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(EdtGitPanel, git_panel_fixture)

BOOST_AUTO_TEST_CASE(AnAssetEditInRes_RefreshesAVisiblePanel)
{
	panel->model().set_visible(true);
	BOOST_REQUIRE(count("vcs.info") == 1u);
	BOOST_REQUIRE(count("vcs.status") == 1u);

	dir.write("project/res/levels/extra.desc", "extra");

	BOOST_TEST(pump_until([this] { return count("vcs.status") == 2u; }));
	BOOST_TEST(count("vcs.info") == 2u);
}

BOOST_AUTO_TEST_CASE(AnAssetEditInRes_WhileHidden_RunsNoGit_AndRefreshesWhenShown)
{
	panel->model().set_visible(true);
	panel->model().set_visible(false);
	const auto before = calls.size();

	dir.write("project/res/levels/extra.desc", "extra");

	BOOST_TEST(pump_until([this] { return !seen.empty(); }));
	BOOST_TEST(calls.size() == before);
	panel->model().set_visible(true);
	BOOST_TEST(count("vcs.status") == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
