#include <boost/test/unit_test.hpp>
#include "edt_world_systems_model.h"
#include <snk/editor/edt_editor_events.h>
#include <boost/json.hpp>
#include <string>
#include <utility>
#include <vector>

// The World Systems panel's model (#549): which world.systems.* commands it runs and what it makes of their results.
namespace {

namespace json = boost::json;

struct model_fixture
{
	struct call
	{
		std::string id;
		json::value args;
	};

	edt::editor_events events;
	std::vector<call> calls;
	bool playing = false;
	bool refuse = false;
	bool no_world = false;
	int lists = 0;
	edt::world_systems_model model{ events, ports() };

	edt::world_systems_model::ports ports()
	{
		edt::world_systems_model::ports out;
		out.invoke = [this](std::string_view id, json::value args) {
			calls.push_back({ std::string(id), args });
			if (id == "world.systems.list") {
				++lists;
				if (no_world) return cmd::result::failure(cmd::codes::not_found, "the scene has no world");
				return cmd::result::success(json::object{ { "world", "main" },
					{ "systems", json::array{ json::object{ { "name", "physics" }, { "known", true }, { "phase", "update" }, { "run_in_edit", false } },
						json::object{ { "name", "gone" }, { "known", false } } } },
					{ "available", json::array{ json::object{ { "name", "audio" }, { "phase", "late" }, { "run_in_edit", true } } } } });
			}
			if (refuse) return cmd::result::failure(cmd::codes::busy, "Stop Play first");
			return cmd::result::success(json::object{ { "generation", 1 } });
		};
		out.playing = [this] { return playing; };
		return out;
	}
};

}

BOOST_AUTO_TEST_SUITE(EdtWorldSystemsModelTests)

BOOST_FIXTURE_TEST_CASE(Refresh_ReadsTheListWithItsUnknownAndAvailableSystems, model_fixture)
{
	model.refresh();
	BOOST_CHECK_EQUAL(model.world(), "main");
	BOOST_REQUIRE_EQUAL(model.systems().size(), 2u);
	BOOST_CHECK_EQUAL(model.systems()[0].phase, "update");
	BOOST_CHECK(model.systems()[0].known);
	BOOST_CHECK(!model.systems()[1].known);
	BOOST_REQUIRE_EQUAL(model.available().size(), 1u);
	BOOST_CHECK_EQUAL(model.available()[0].name, "audio");
	BOOST_CHECK(model.available()[0].run_in_edit);
}

BOOST_FIXTURE_TEST_CASE(RefreshIfChanged_ReadsOnceUntilTheDocumentChanges, model_fixture)
{
	model.refresh_if_changed();
	model.refresh_if_changed();
	BOOST_CHECK_EQUAL(lists, 1);
	events.document_changed.emit(edt::document_id{}, 2);
	model.refresh_if_changed();
	BOOST_CHECK_EQUAL(lists, 2);
	events.active_world_changed.emit(edt::document_id{});
	model.refresh_if_changed();
	BOOST_CHECK_EQUAL(lists, 3);
}

BOOST_FIXTURE_TEST_CASE(Edits_RunTheCommandsOfTheShownWorld_AndReadTheListAgain, model_fixture)
{
	model.refresh();
	calls.clear();
	BOOST_CHECK(model.add("audio"));
	BOOST_CHECK(model.remove("gone"));
	BOOST_CHECK(model.move("physics", 1));
	BOOST_REQUIRE_EQUAL(calls.size(), 6u);
	BOOST_CHECK_EQUAL(calls[0].id, "world.systems.add");
	BOOST_CHECK_EQUAL(calls[0].args.as_object().at("world").as_string(), "main");
	BOOST_CHECK_EQUAL(calls[0].args.as_object().at("system").as_string(), "audio");
	BOOST_CHECK_EQUAL(calls[1].id, "world.systems.list");
	BOOST_CHECK_EQUAL(calls[2].id, "world.systems.remove");
	BOOST_CHECK_EQUAL(calls[4].id, "world.systems.move");
	BOOST_CHECK_EQUAL(calls[4].args.as_object().at("to").as_int64(), 1);
	BOOST_CHECK(model.error().empty());
}

BOOST_FIXTURE_TEST_CASE(ARefusedEdit_KeepsItsReason_UntilTheNextOneGoesThrough, model_fixture)
{
	model.refresh();
	refuse = true;
	BOOST_CHECK(!model.add("audio"));
	BOOST_CHECK_EQUAL(model.error(), "Stop Play first");
	refuse = false;
	BOOST_CHECK(model.add("audio"));
	BOOST_CHECK(model.error().empty());
}

BOOST_FIXTURE_TEST_CASE(AListError_GoesAwayWhenTheListIsReadAgain_ButARefusalIsNotWipedByTheRead, model_fixture)
{
	no_world = true;
	model.refresh();
	BOOST_CHECK_EQUAL(model.error(), "the scene has no world");
	BOOST_CHECK(model.systems().empty());
	no_world = false;
	model.refresh();
	BOOST_CHECK(model.error().empty());
	BOOST_CHECK_EQUAL(model.systems().size(), 2u);
	refuse = true;
	BOOST_CHECK(!model.add("audio"));
	BOOST_CHECK_EQUAL(model.error(), "Stop Play first");
}

BOOST_FIXTURE_TEST_CASE(Locked_FollowsPlay, model_fixture)
{
	BOOST_CHECK(!model.locked());
	playing = true;
	BOOST_CHECK(model.locked());
}

BOOST_AUTO_TEST_SUITE_END()
