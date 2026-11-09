#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/common/logger/engine_log.h>
#include <snk/common/logger/engine_log_sinks.h>
#include <snk/config/cfg_variables.h>
#include <snk/resource/resources/res_resource_text.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_start_level.h>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #656: the start level is reported loaded only once the loads it started are done, or after scene/level_settle_frames.
namespace {

constexpr std::string_view k_level = R"json({"__type":"level_desc","name":"lv","worlds":[
	{"__type":"world_desc","name":"w0","systems":[]}]})json";

// Holds every request until release(): a load the level started that is still on its way.
class gated_resolver : public test_util::ram_resolver
{
public:
	explicit gated_resolver(std::string_view prt) : ram_resolver(prt) {}

	async_raw_data resolve(const res::tag& tag) const override
	{
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		std::lock_guard lock(mutex);
		held.emplace_back(block, files.at(tag));
		return block;
	}

	void release()
	{
		decltype(held) taken;
		{
			std::lock_guard lock(mutex);
			taken.swap(held);
		}
		for (auto& [block, data] : taken) {
			block->set_ready(std::move(data));
		}
	}

	mutable std::mutex mutex;
	mutable std::vector<std::pair<std::shared_ptr<core::res::res_control_block<std::vector<std::byte>>>, std::vector<std::byte>>> held;
};

struct start_level_fixture : test_util::scene_env
{
	start_level_fixture() : mgr(*rs, *assembler, sf)
	{
		gate = &rs->registrate_resolver<gated_resolver>("gate");
		gate->files[res::tag{ "gate://nested.txt" }] = test_util::to_bytes("nested");
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			std::lock_guard lock(log_mutex);
			log.push_back({ level, std::string{ message } });
		});
	}

	~start_level_fixture()
	{
		gate->release();
		pump();
		cfg::set_variable("scene/level_settle_frames", "600");
	}

	std::size_t count(std::string_view text, engine::log_level level)
	{
		std::lock_guard lock(log_mutex);
		return static_cast<std::size_t>(std::ranges::count_if(log, [&](const auto& line) {
			return line.first == level && line.second.find(text) != std::string::npos;
		}));
	}

	ecs::system_factory sf;
	scn::context_manager mgr;
	gated_resolver* gate = nullptr;
	std::mutex log_mutex;
	std::vector<std::pair<engine::log_level, std::string>> log;
	engine::log_sink_handle sink;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnStartLevelTests, start_level_fixture)

BOOST_AUTO_TEST_CASE(a_level_is_not_loaded_while_a_load_it_started_is_in_flight)
{
	const res::tag level = put("lv.desc", k_level);
	scn::start_level_loader loader{ mgr, *rs, tasks };
	loader.begin(level);
	pump();
	// What a mesh node of the level asks for while the set is built: held by the gate.
	const auto nested = rs->require<res::text_resource>(res::tag{ "gate://nested.txt" });

	BOOST_TEST((loader.poll() == scn::start_level_loader::state::settling));
	BOOST_TEST((loader.poll() == scn::start_level_loader::state::settling));
	BOOST_TEST(count("level loaded:", engine::log_level::info) == 0u);
	BOOST_TEST(mgr.level_set().size() == 1u); // the set is already built: only the report waits

	gate->release();
	pump();
	BOOST_REQUIRE(nested.is_ready());
	BOOST_TEST((loader.poll() == scn::start_level_loader::state::loaded));
	BOOST_TEST(count("level loaded: " + std::string{ level.view() }, engine::log_level::info) == 1u);
	BOOST_TEST(count("still pending", engine::log_level::warn) == 0u);
	BOOST_TEST((loader.poll() == scn::start_level_loader::state::loaded));
	BOOST_TEST(count("level loaded:", engine::log_level::info) == 1u);
}

BOOST_AUTO_TEST_CASE(a_level_whose_loads_never_end_is_reported_loaded_after_the_settle_frames)
{
	cfg::set_variable("scene/level_settle_frames", "3");
	const res::tag level = put("lv.desc", k_level);
	scn::start_level_loader loader{ mgr, *rs, tasks };
	loader.begin(level);
	pump();
	const auto nested = rs->require<res::text_resource>(res::tag{ "gate://nested.txt" });

	BOOST_TEST((loader.poll() == scn::start_level_loader::state::settling));
	BOOST_TEST((loader.poll() == scn::start_level_loader::state::settling));
	BOOST_TEST((loader.poll() == scn::start_level_loader::state::loaded));
	BOOST_TEST(count("still pending after 3 frames", engine::log_level::warn) == 1u);
	BOOST_TEST(count("level loaded:", engine::log_level::info) == 1u);
	BOOST_TEST(!nested.is_ready());
}

BOOST_AUTO_TEST_CASE(a_level_with_nothing_left_to_load_is_loaded_on_the_first_poll)
{
	const res::tag level = put("lv.desc", k_level);
	scn::start_level_loader loader{ mgr, *rs, tasks };
	loader.begin(level);
	pump();
	BOOST_TEST((loader.poll() == scn::start_level_loader::state::loaded));
	BOOST_TEST(count("level loaded:", engine::log_level::info) == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
