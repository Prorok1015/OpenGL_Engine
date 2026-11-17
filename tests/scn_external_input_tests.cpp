#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/ecs/ecs_system_registry.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <chrono>
#include <thread>
#include <vector>

// The way in for code outside the phases (WORLD_MESSAGING 3 and 9, W5 #273): context_manager::post and
// context_manager::blackboard. The editor and the game shell call them between frames, on the thread that
// steps the manager; the channel has no lock of its own. A post or a write is seen by the contexts at the next
// exchange of the set (lag of one frame), never inside the call and never twice. W1's tests cover the order of
// delivery and the limits; these cover the entry itself.

namespace {

struct command
{
	int value = 0;
};

constexpr ecs::bb_key<int> k_setting{ "settings.quality" };
constexpr ecs::bb_key<float> k_volume{ "settings.volume" };

constexpr auto one_frame = std::chrono::duration<float>(0.001f);

// What the context saw in each of its frames.
struct seen
{
	std::vector<std::vector<int>> commands;
	std::vector<int> setting; // -1: absent
};

void sys_observe(entt::registry& reg)
{
	auto& s = reg.ctx().get<seen>();
	std::vector<int> values;
	for (const auto& m : reg.ctx().get<ecs::inbox<command>>().messages()) values.push_back(m.payload.value);
	s.commands.push_back(std::move(values));
	s.setting.push_back(reg.ctx().get<ecs::blackboard_reader>().get_or(k_setting, -1));
}

struct input_fixture : test_util::scene_env
{
	input_fixture()
		: mgr(*rs, *assembler, sf)
	{
		sf.register_system<&sys_observe>("observe", { .phase = ecs::phase::update, .run_in_edit = true });
		mgr.channels().add<command>();
		mgr.create_set("play");
	}

	scn::world_context& add(std::string_view set, std::string_view name)
	{
		auto& context = mgr.create_context(set, name, scn::context_kind::runtime, { "observe" });
		context.state().ctx().emplace<seen>();
		return context;
	}

	static const seen& seen_by(scn::world_context& context) { return context.state().ctx().get<seen>(); }

	ecs::system_factory sf;
	scn::context_manager mgr;
};

}

BOOST_FIXTURE_TEST_SUITE(ScnExternalInputTests, input_fixture)

BOOST_AUTO_TEST_CASE(Post_IsDeliveredAtTheNextExchangeOnceAndNotInsideTheCall)
{
	auto& w = add("level", "w");
	mgr.post("level", { "w" }, command{ 1 });
	BOOST_TEST(w.state().ctx().get<ecs::inbox<command>>().messages().empty()); // nothing happens inside post

	mgr.update(one_frame);
	mgr.update(one_frame);
	BOOST_REQUIRE_EQUAL(seen_by(w).commands.size(), 2u);
	BOOST_TEST((seen_by(w).commands[0] == std::vector<int>{ 1 }));
	BOOST_TEST(seen_by(w).commands[1].empty()); // an inbox lives one frame
}

BOOST_AUTO_TEST_CASE(Post_BroadcastReachesEveryContextOfTheSetAndNoOtherSet)
{
	auto& a = add("level", "a");
	auto& b = add("level", "b");
	auto& other = add("play", "a");
	mgr.post("level", {}, command{ 7 });
	mgr.update(one_frame);
	BOOST_TEST((seen_by(a).commands[0] == std::vector<int>{ 7 }));
	BOOST_TEST((seen_by(b).commands[0] == std::vector<int>{ 7 }));
	BOOST_TEST(seen_by(other).commands[0].empty());
}

BOOST_AUTO_TEST_CASE(Post_KeepsTheOrderOfTheCalls)
{
	auto& w = add("level", "w");
	for (int i = 1; i <= 5; ++i) mgr.post("level", { "w" }, command{ i });
	mgr.update(one_frame);
	BOOST_TEST((seen_by(w).commands[0] == std::vector<int>{ 1, 2, 3, 4, 5 }));
}

BOOST_AUTO_TEST_CASE(Blackboard_AnExternalWriteIsSeenFromTheNextExchangeAndStays)
{
	auto& w = add("level", "w");
	auto& writer = mgr.blackboard("level");
	writer.set(k_setting, 3);
	BOOST_TEST(mgr.get_set("level").channel().reader().find(k_setting) == nullptr); // not visible inside the call

	mgr.update(one_frame);
	mgr.update(one_frame);
	BOOST_TEST((seen_by(w).setting == std::vector<int>{ 3, 3 }));

	writer.set(k_setting, 4); // the same writer stays valid; the new value replaces the old one
	mgr.update(one_frame);
	BOOST_TEST(seen_by(w).setting.back() == 4);
	BOOST_TEST(&mgr.blackboard("level") == &writer);
}

BOOST_AUTO_TEST_CASE(Blackboard_EachSetHasItsOwnEntry)
{
	auto& level = add("level", "w");
	auto& play = add("play", "w");
	mgr.blackboard("play").set(k_setting, 9);
	mgr.update(one_frame);
	BOOST_TEST(seen_by(level).setting.back() == -1);
	BOOST_TEST(seen_by(play).setting.back() == 9);
	BOOST_TEST(&mgr.blackboard("level") != &mgr.blackboard("play"));
}

BOOST_AUTO_TEST_CASE(Frozen_APostWaitsInTheEditorUntilTheSetRunsAgain)
{
	auto& w = add("level", "w");
	mgr.set_mode("level", ecs::run_mode::edit);
	mgr.post("level", { "w" }, command{ 5 });
	mgr.blackboard("level").set(k_setting, 8);
	mgr.update(one_frame);
	mgr.update(one_frame);
	// Edit: the contexts run (observe is run_in_edit) but the channel does not exchange
	BOOST_TEST(seen_by(w).commands[0].empty());
	BOOST_TEST(seen_by(w).commands[1].empty());
	BOOST_TEST((seen_by(w).setting == std::vector<int>{ -1, -1 }));

	mgr.set_mode("level", ecs::run_mode::play);
	mgr.update(one_frame);
	BOOST_TEST((seen_by(w).commands[2] == std::vector<int>{ 5 })); // delivered once, not lost, not repeated
	BOOST_TEST(seen_by(w).setting[2] == 8);
	mgr.update(one_frame);
	BOOST_TEST(seen_by(w).commands[3].empty());
}

BOOST_AUTO_TEST_CASE(Threads_AWorkerMayPostWhileTheManagerIsNotSteppedAndJoinsBeforeTheNextFrame)
{
	// The contract: one thread at a time touches the entry, and the hand-over is a join (or the main-thread
	// queue). There is no lock in the channel; this is the supported way to use it from a job.
	auto& w = add("level", "w");
	std::thread worker([this] {
		for (int i = 1; i <= 100; ++i) mgr.post("level", { "w" }, command{ i });
		mgr.blackboard("level").set(k_setting, 100);
	});
	worker.join();

	mgr.update(one_frame);
	BOOST_REQUIRE_EQUAL(seen_by(w).commands[0].size(), 100u);
	for (int i = 0; i < 100; ++i) BOOST_TEST(seen_by(w).commands[0][static_cast<size_t>(i)] == i + 1);
	BOOST_TEST(seen_by(w).setting[0] == 100);
}

BOOST_AUTO_TEST_CASE(Types_AnExternalWriterKeepsTheTypeOfTheKeyPerName)
{
	add("level", "w");
	mgr.blackboard("level").set(k_volume, 0.5f);
	mgr.blackboard("level").set(k_setting, 2);
	mgr.update(one_frame);
	const auto reader = mgr.get_set("level").channel().reader();
	BOOST_REQUIRE(reader.find(k_volume));
	BOOST_TEST(*reader.find(k_volume) == 0.5f);
	BOOST_REQUIRE(reader.find(k_setting));
	BOOST_TEST(*reader.find(k_setting) == 2);
}

BOOST_AUTO_TEST_SUITE_END()
