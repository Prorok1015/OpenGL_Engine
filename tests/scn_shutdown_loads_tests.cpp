#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/common/logger/engine_log.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/desc/desc_base.hpp>
#include <snk/resource/res_tag.h>
#include <snk/scene/level/scn_context_manager.h>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// #653: a load still in flight when the engine shuts down (the game host exits on the frame its level is ready, while
// the level's material and geometry descs still load) must not deserialize after the scene types are unregistered.
BOOST_AUTO_TEST_SUITE(ScnShutdownLoadsTests)

BOOST_AUTO_TEST_CASE(a_desc_load_in_flight_at_shutdown_is_cancelled_before_its_type_is_unregistered)
{
	std::mutex m;
	std::vector<std::string> problems;
	auto sink = engine::add_log_sink([&](std::string_view, engine::log_level level, std::string_view message) {
		if (level == engine::log_level::error || message.find("still referenced") != std::string_view::npos) {
			std::lock_guard lock(m);
			problems.emplace_back(message);
		}
	});

	{
		test_util::null_engine_fixture engine;
		scn::scene_layer* layer = engine.storage.require<scn::context_manager>().game_layer();
		BOOST_REQUIRE(layer != nullptr);
		// The manual task system runs nothing until pumped: the load stays in flight.
		auto material = layer->resources().require<desc::desc_base>(res::tag{ "res://base_material.desc" });
		BOOST_REQUIRE(!material.is_ready());

		engine.unregister_scene_types();
		test_util::pump(engine.tasks);

		BOOST_TEST(!material.is_ready());
		BOOST_TEST(material.error_text().find("cancelled") != std::string::npos, "error: " << material.error_text());
	}

	std::lock_guard lock(m);
	for (const auto& problem : problems) {
		BOOST_TEST_MESSAGE("logged: " << problem);
	}
	BOOST_TEST(problems.empty());
}

BOOST_AUTO_TEST_SUITE_END()
