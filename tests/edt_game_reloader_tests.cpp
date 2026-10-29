#include <boost/test/unit_test.hpp>
#include "edt_game_reloader.h"
#include <snk/editor/edt_action_registry.h>
#include <snk/editor/edt_dialog_service.h>
#include <snk/editor/edt_editor_events.h>
#include <boost/json/object.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Build > Reload Game Module and the reload after a build (#435): game_reloader over a fake host.

namespace {

struct reloader_fixture
{
	reloader_fixture()
	{
		dialogs.set_registrar([](std::string_view, std::function<bool()>) {});
	}

	edt::game_reloader_options options()
	{
		edt::game_reloader_options out;
		out.modules = [this] { return modules; };
		out.build_output = [this](const std::string& module) -> std::optional<std::filesystem::path> {
			if (!has_build) return std::nullopt;
			return std::filesystem::path{ "build/bin" } / (module + ".dll");
		};
		out.reload = [this](const std::string& module) {
			reloaded.push_back(module);
			return next;
		};
		out.building = [this] { return building; };
		out.events = &events;
		out.next_frame = [this](const std::string& id, std::function<void()> fn) { scheduled.emplace_back(id, std::move(fn)); };
		return out;
	}

	void make() { reloader = std::make_unique<edt::game_reloader>(dialogs, actions, options()); }

	edt::action_result invoke(boost::json::object args = {})
	{
		return actions.invoke(edt::game_reloader::k_action_id, args);
	}

	// The messages shown to the person.
	std::vector<std::string> messages() const
	{
		std::vector<std::string> out;
		for (const auto& info : dialogs.list()) {
			if (info.kind == "message") out.push_back(info.text);
		}
		return out;
	}

	edt::game_module_build_result built(bool reload) const
	{
		edt::game_module_build_result result;
		result.module = "game";
		result.path = "build/bin/game.dll";
		result.configuration = "debuggame";
		result.reload = reload;
		return result;
	}

	edt::action_registry actions;
	edt::dialog_service dialogs;
	edt::editor_events events;
	std::vector<std::string> modules{ "game" };
	bool has_build = true;
	bool building = false;
	core::reload_report next{ core::reload_status::reloaded, {} };
	std::vector<std::string> reloaded;
	std::vector<std::pair<std::string, std::function<void()>>> scheduled;
	std::unique_ptr<edt::game_reloader> reloader;
};

}

BOOST_FIXTURE_TEST_SUITE(edt_game_reloader_tests, reloader_fixture)

BOOST_AUTO_TEST_CASE(the_action_reloads_the_built_module_and_returns_the_status)
{
	make();
	BOOST_TEST(reloader->disabled_reason().empty());

	const auto outcome = invoke();
	BOOST_REQUIRE_MESSAGE(outcome.ok, outcome.error);
	BOOST_TEST(outcome.value.as_object().at("status").as_string() == "reloaded");
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
	BOOST_TEST(messages().empty());
}

BOOST_AUTO_TEST_CASE(without_a_build_the_action_is_disabled)
{
	has_build = false;
	make();
	BOOST_TEST(!reloader->disabled_reason().empty());
	BOOST_TEST(!invoke().ok);
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(while_a_build_runs_the_action_is_disabled)
{
	building = true;
	make();
	BOOST_TEST(reloader->disabled_reason() == "A build is running");
	BOOST_TEST(!invoke().ok);
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(a_build_not_taken_is_shown_with_its_reason)
{
	make();
	for (const auto status : { core::reload_status::refused, core::reload_status::rolled_back, core::reload_status::lost }) {
		next = { status, std::string{ "because of " } + std::string{ core::to_string(status) } };
		const auto outcome = invoke();
		BOOST_REQUIRE(outcome.ok);
		BOOST_TEST(outcome.value.as_object().at("status").as_string() == core::to_string(status));
	}
	const auto shown = messages();
	BOOST_REQUIRE(shown.size() == 3u);
	BOOST_TEST(shown[0].find("because of refused") != std::string::npos);
	BOOST_TEST(shown[0].find("running version is kept") != std::string::npos);
	BOOST_TEST(shown[1].find("rolled back") != std::string::npos);
	BOOST_TEST(shown[2].find("because of lost") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_build_marked_for_reload_is_reloaded_on_the_next_frame)
{
	make();
	events.game_module_built.emit(built(true));
	BOOST_TEST(reloaded.empty()); // not inside the job pump that finished the build
	BOOST_REQUIRE(scheduled.size() == 1u);
	BOOST_TEST(scheduled.front().first == "edt/reload_game_module/game");

	scheduled.front().second();
	BOOST_TEST(reloaded == std::vector<std::string>{ "game" });
}

BOOST_AUTO_TEST_CASE(a_build_not_marked_for_reload_is_left_alone)
{
	make();
	events.game_module_built.emit(built(false));
	BOOST_TEST(scheduled.empty());
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_CASE(a_reload_scheduled_before_the_reloader_went_does_nothing)
{
	make();
	events.game_module_built.emit(built(true));
	BOOST_REQUIRE(scheduled.size() == 1u);
	reloader.reset();
	scheduled.front().second();
	BOOST_TEST(reloaded.empty());
}

BOOST_AUTO_TEST_SUITE_END()
