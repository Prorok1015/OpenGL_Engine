#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/ecs/ecs_channel.hpp>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/common/logger/engine_log.h>
#include <chrono>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The data channel of a context set (WORLD_MESSAGING W1): blackboard + mailbox with a lag of one
// frame, exchanged once at the start of the set's frame. Contexts are driven by a scripted system
// that reaches the channel through its own registry ctx, like any production system would.
// (A key read as another type than it was written asserts and aborts, so that check is not tested.)

namespace {

struct msg
{
	int value = 0;
	bool operator==(const msg&) const = default;
};

struct add_value { int amount = 0; };

constexpr ecs::bb_key<int> k_shared{ "test.shared" };
constexpr ecs::bb_key<int> k_frame{ "test.frame" };
constexpr ecs::bb_key<int> k_ext{ "test.ext" };

constexpr auto one_frame = std::chrono::duration<float>(0.001f);

// What a context saw, frame by frame.
struct probe
{
	std::vector<std::vector<int>> inbox_values; // per script call
	std::vector<std::vector<std::string>> inbox_from;
	std::vector<int> shared_seen; // -1: key absent
	std::vector<std::string> lines;
};

struct script
{
	std::function<void(entt::registry&, int)> fn;
	int frame = 0;
};

void record(entt::registry& reg)
{
	auto& p = reg.ctx().emplace<probe>();
	const auto& in = reg.ctx().get<ecs::inbox<msg>>();
	std::vector<int> values;
	std::vector<std::string> from;
	std::string line;
	for (const auto& m : in.messages()) {
		values.push_back(m.payload.value);
		from.push_back(m.from);
		line += std::format("[{}>{}#{}:{}]", m.from.empty() ? "ext" : m.from, m.to.context.empty() ? "*" : m.to.context, m.seq, m.payload.value);
	}
	const int seen = reg.ctx().get<ecs::blackboard_reader>().get_or(k_shared, -1);
	p.inbox_values.push_back(std::move(values));
	p.inbox_from.push_back(std::move(from));
	p.shared_seen.push_back(seen);
	p.lines.push_back(std::format("bb={} in={}", seen, line));
}

void run_script(entt::registry& reg)
{
	auto& s = reg.ctx().get<script>();
	s.fn(reg, s.frame++);
}

// Same body, two phases: the scenario must not depend on which of the two runs first.
void sys_script_early(entt::registry& reg) { run_script(reg); }
void sys_script_late(entt::registry& reg) { run_script(reg); }

struct reader_state
{
	int seen = 0;
	bool asked = false;
};

// Channel resources as plain system arguments (no invoker support needed).
void sys_typed_reader(const ecs::blackboard_reader& board, ecs::outbox<add_value>& out, reader_state& state)
{
	const int* value = board.find(k_shared);
	if (!value) return;
	state.seen = *value;
	if (!state.asked) {
		state.asked = true;
		out.send({ "owner" }, add_value{ 5 });
	}
}

struct shared_state { int value = 0; };

void sys_owner(shared_state& state, const ecs::inbox<add_value>& in, ecs::blackboard_writer& board)
{
	for (const auto& m : in.messages()) state.value += m.payload.amount;
	board.set(k_shared, state.value);
}

struct channel_fixture : test_util::scene_env
{
	channel_fixture()
		: mgr(*rs, *assembler, sf)
	{
		sf.register_system<&sys_script_early>("script_early", { .phase = ecs::phase::input, .run_in_edit = true });
		sf.register_system<&sys_script_late>("script_late", { .phase = ecs::phase::late_update, .run_in_edit = true });
		sf.register_system<&sys_typed_reader>("typed_reader", { .phase = ecs::phase::update, .run_in_edit = true });
		sf.register_system<&sys_owner>("owner", { .phase = ecs::phase::update, .run_in_edit = true });
		mgr.channels().add<msg>();
		mgr.channels().add<add_value>();
	}

	std::unique_ptr<scn::context_set> make_set(std::string name = "level")
	{
		auto set = std::make_unique<scn::context_set>(std::move(name), nullptr);
		set->channel().types().add<msg>();
		set->channel().types().add<add_value>();
		return set;
	}

	scn::world_context& add(scn::context_set& set, std::string_view name, uint32_t id, std::vector<std::string> systems,
							std::function<void(entt::registry&, int)> fn = {})
	{
		auto& context = set.create_context(name, id);
		set.add_systems(context, systems, sf);
		if (fn) context.state().ctx().emplace<script>(script{ std::move(fn), 0 });
		return context;
	}

	static const probe& seen(scn::world_context& context) { return context.state().ctx().get<probe>(); }

	// Both contexts message each other and write one key; a run is reduced to text.
	std::vector<std::string> run_pingpong(bool a_early)
	{
		auto set = make_set();
		auto body = [](std::string other, int id) {
			return [other, id](entt::registry& reg, int frame) {
				record(reg);
				auto& out = reg.ctx().get<ecs::outbox<msg>>();
				out.send({ other }, msg{ frame * 10 + id });
				out.send({}, msg{ frame * 10 + id + 5 });
				reg.ctx().get<ecs::blackboard_writer>().set(k_shared, frame * 100 + id);
				reg.ctx().get<ecs::blackboard_writer>().set(k_frame, frame);
			};
		};
		auto& a = add(*set, "a", 0, { a_early ? "script_early" : "script_late" }, body("b", 0));
		auto& b = add(*set, "b", 1, { a_early ? "script_late" : "script_early" }, body("a", 1));

		std::vector<std::string> transcript;
		for (int frame = 0; frame < 6; ++frame) {
			set->update(one_frame);
			for (auto* c : { &a, &b }) transcript.push_back(std::format("f{} {} {}", frame, c->get_name(), seen(*c).lines.back()));
			for (const auto& [key, entry] : set->channel().blackboard().committed()) {
				transcript.push_back(std::format("f{} {}={}", frame, key, *static_cast<const int*>(entry.value.get())));
			}
		}
		return transcript;
	}

	ecs::system_factory sf;
	scn::context_manager mgr;
};

struct log_capture
{
	log_capture()
	{
		sink = engine::add_log_sink([this](std::string_view category, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::warn) warnings.push_back(std::format("{}: {}", category, message));
		});
	}

	bool has(std::string_view part) const
	{
		for (const auto& w : warnings) {
			if (w.find(part) != std::string::npos) return true;
		}
		return false;
	}

	std::vector<std::string> warnings;
	engine::log_sink_handle sink; // last: removed first
};

}

BOOST_FIXTURE_TEST_SUITE(ScnSetChannelTests, channel_fixture)

// ---------------------------------------------------------------- lag

BOOST_AUTO_TEST_CASE(Lag_AWriteIsVisibleExactlyOneFrameLater)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		reg.ctx().get<ecs::blackboard_writer>().set(k_shared, 100 + frame);
	});

	for (int i = 0; i < 4; ++i) set->update(one_frame);

	BOOST_TEST((seen(a).shared_seen == std::vector<int>{ -1, 100, 101, 102 }));
}

BOOST_AUTO_TEST_CASE(Lag_InboxHoldsTheMessagesOfThePreviousFrameForTheWholeFrame)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		reg.ctx().get<ecs::outbox<msg>>().send({ "a" }, msg{ frame });
	});
	auto& late = add(*set, "late", 1, { "script_late" }, [](entt::registry& reg, int) { record(reg); });

	set->update(one_frame);
	BOOST_TEST(seen(a).inbox_values[0].empty()); // nothing was sent before the first frame
	set->update(one_frame);
	set->update(one_frame);

	BOOST_TEST((seen(a).inbox_values[1] == std::vector<int>{ 0 }));
	BOOST_TEST((seen(a).inbox_values[2] == std::vector<int>{ 1 }));
	// `late` never receives the addressed messages
	for (const auto& v : seen(late).inbox_values) BOOST_TEST(v.empty());
}

BOOST_AUTO_TEST_CASE(Lag_ChannelResourcesWorkAsSystemArguments)
{
	auto set = make_set();
	add(*set, "owner", 0, { "owner" });
	auto& reader = add(*set, "reader", 1, { "typed_reader" });

	for (int i = 0; i < 4; ++i) set->update(one_frame);

	// f0: owner publishes 0; f1: reader sees it and asks for +5; f2: owner applies; f3: reader sees 5
	BOOST_TEST(reader.state().ctx().get<reader_state>().seen == 5);
	BOOST_TEST(set->get("owner").state().ctx().get<shared_state>().value == 5);
}

BOOST_AUTO_TEST_CASE(Channel_ReplacesCrossWorldBindRes)
{
	// Scenario of ecs_invoker_tests sys_read/write_other_world (bind_res<1, shared_state>): one
	// context reads a value of another one and changes it. Here through the channel only.
	auto set = make_set();
	auto& owner = add(*set, "owner", 1, { "owner" });
	owner.state().ctx().emplace<shared_state>().value = 7;
	auto& other = add(*set, "other", 0, { "typed_reader" });

	for (int i = 0; i < 2; ++i) set->update(one_frame);
	BOOST_TEST(other.state().ctx().get<reader_state>().seen == 7); // read across contexts, one frame late

	for (int i = 0; i < 3; ++i) set->update(one_frame);
	BOOST_TEST(owner.state().ctx().get<shared_state>().value == 12); // written across contexts
	BOOST_TEST(other.state().ctx().get<reader_state>().seen == 12);
}

// ---------------------------------------------------------------- delivery

BOOST_AUTO_TEST_CASE(Order_ExternalFirstThenContextsByAscendingIdThenSend)
{
	auto set = make_set();
	auto send_two = [](int base) {
		return [base](entt::registry& reg, int frame) {
			record(reg);
			if (frame != 0) return;
			auto& out = reg.ctx().get<ecs::outbox<msg>>();
			out.send({}, msg{ base + 1 });
			out.send({}, msg{ base + 2 });
		};
	};
	// ids 5 and 2: creation order is not id order
	auto& hi = add(*set, "hi", 5, { "script_early" }, send_two(50));
	auto& lo = add(*set, "lo", 2, { "script_early" }, send_two(20));

	set->update(one_frame);
	set->channel().post_external<msg>({}, msg{ 1 });
	set->channel().post_external<msg>({}, msg{ 2 });
	set->update(one_frame);

	// a broadcast does not come back to its sender
	BOOST_TEST((seen(hi).inbox_values[1] == std::vector<int>{ 1, 2, 21, 22 }));
	BOOST_TEST((seen(lo).inbox_values[1] == std::vector<int>{ 1, 2, 51, 52 }));
	BOOST_TEST((seen(lo).inbox_from[1] == std::vector<std::string>{ "", "", "hi", "hi" }));
}

BOOST_AUTO_TEST_CASE(Delivery_BroadcastReachesOthersNotTheSenderSelfAddressReachesTheSender)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame != 0) return;
		auto& out = reg.ctx().get<ecs::outbox<msg>>();
		out.send({ "b" }, msg{ 1 });
		out.send({}, msg{ 2 });
		out.send({ "a" }, msg{ 3 });
	});
	auto& b = add(*set, "b", 1, { "script_early" }, [](entt::registry& reg, int) { record(reg); });
	auto& c = add(*set, "c", 2, { "script_early" }, [](entt::registry& reg, int) { record(reg); });

	set->update(one_frame);
	set->update(one_frame);

	BOOST_TEST((seen(a).inbox_values[1] == std::vector<int>{ 3 })); // only the explicit self-address
	BOOST_TEST((seen(b).inbox_values[1] == std::vector<int>{ 1, 2 }));
	BOOST_TEST((seen(c).inbox_values[1] == std::vector<int>{ 2 }));
}

BOOST_AUTO_TEST_CASE(Delivery_NoAddresseeIsDroppedAndLogged)
{
	log_capture log;
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame == 0) reg.ctx().get<ecs::outbox<msg>>().send({ "nobody" }, msg{ 9 });
	});

	set->update(one_frame);
	set->update(one_frame);
	set->update(one_frame);

	BOOST_TEST(log.has("scene: "));
	BOOST_TEST(log.has("'nobody'"));
	for (const auto& v : seen(a).inbox_values) BOOST_TEST(v.empty());
}

BOOST_AUTO_TEST_CASE(Delivery_AMessageTypeThatIsNotRegisteredHasNoEndpoints)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry&, int) {});
	set->update(one_frame);

	BOOST_TEST(a.state().ctx().find<ecs::inbox<msg>>() != nullptr);
	BOOST_TEST(a.state().ctx().find<ecs::inbox<int>>() == nullptr);
	BOOST_TEST(!set->channel().types().contains<int>());
}

BOOST_AUTO_TEST_CASE(Delivery_ATypeRegisteredLaterGetsEndpointsOnTheNextFrame)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry&, int) {});
	set->update(one_frame);
	BOOST_TEST(a.state().ctx().find<ecs::inbox<float>>() == nullptr);

	set->channel().types().add<float>();
	set->update(one_frame);

	BOOST_TEST(a.state().ctx().find<ecs::inbox<float>>() != nullptr);
	BOOST_TEST(a.state().ctx().get<ecs::outbox<float>>().bound());
}

BOOST_AUTO_TEST_CASE(Limit_MessagesPastThePerFrameLimitAreDroppedWithAWarning)
{
	log_capture log;
	auto set = make_set();
	set->channel().set_message_limit(3);
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame != 0) return;
		for (int i = 0; i < 10; ++i) reg.ctx().get<ecs::outbox<msg>>().send({ "a" }, msg{ i });
	});

	set->update(one_frame);
	set->update(one_frame);

	BOOST_TEST((seen(a).inbox_values[1] == std::vector<int>{ 0, 1, 2 }));
	BOOST_TEST(log.has("is full"));
	BOOST_TEST(set->channel().message_limit() == 3u);
	BOOST_TEST(scn::set_channel::default_message_limit == 1024u);
}

BOOST_AUTO_TEST_CASE(Limit_ExternalPostsAreLimitedToo)
{
	log_capture log;
	auto set = make_set();
	set->channel().set_message_limit(2);
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int) { record(reg); });
	set->update(one_frame);
	for (int i = 0; i < 5; ++i) set->channel().post_external<msg>({}, msg{ i });
	set->update(one_frame);

	BOOST_TEST((seen(a).inbox_values[1] == std::vector<int>{ 0, 1 }));
	BOOST_TEST(log.has("external posts"));
}

// ---------------------------------------------------------------- blackboard

BOOST_AUTO_TEST_CASE(Blackboard_TheGreaterContextIdWinsWithAWarning)
{
	log_capture log;
	auto set = make_set();
	auto write = [](int value) {
		return [value](entt::registry& reg, int frame) {
			record(reg);
			if (frame == 0) reg.ctx().get<ecs::blackboard_writer>().set(k_shared, value);
		};
	};
	// the lower id runs last: the result must not depend on the order of writes
	add(*set, "hi", 7, { "script_early" }, write(70));
	auto& lo = add(*set, "lo", 3, { "script_late" }, write(30));

	set->update(one_frame);
	set->update(one_frame);

	BOOST_TEST(seen(lo).shared_seen[1] == 70);
	BOOST_TEST(log.has("test.shared"));
}

BOOST_AUTO_TEST_CASE(Blackboard_ExternalWritesLoseAgainstContexts)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame == 0) reg.ctx().get<ecs::blackboard_writer>().set(k_shared, 1);
	});
	set->channel().external_blackboard().set(k_ext, 5);

	set->update(one_frame);
	BOOST_TEST(*set->channel().reader().find(k_ext) == 5); // an external write is visible at the next exchange

	set->channel().external_blackboard().set(k_shared, 99); // same frame as the context's write
	set->update(one_frame);

	BOOST_TEST(seen(a).shared_seen[1] == 1);
}

BOOST_AUTO_TEST_CASE(Blackboard_KeepsTheLastValueUntilItIsReplaced)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame == 0) reg.ctx().get<ecs::blackboard_writer>().set(k_shared, 4);
	});
	for (int i = 0; i < 5; ++i) set->update(one_frame);

	BOOST_TEST((seen(a).shared_seen == std::vector<int>{ -1, 4, 4, 4, 4 }));
}

BOOST_AUTO_TEST_CASE(Blackboard_ASnapshotDoesNotChangeDuringTheFrame)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early", "script_late" }, [](entt::registry& reg, int call) {
		record(reg);
		reg.ctx().get<ecs::blackboard_writer>().set(k_shared, call);
	});
	for (int i = 0; i < 3; ++i) set->update(one_frame);

	// both calls of one frame see the same value
	const auto& s = seen(a).shared_seen;
	BOOST_REQUIRE(s.size() == 6u);
	for (size_t frame = 0; frame < 3; ++frame) BOOST_TEST(s[frame * 2] == s[frame * 2 + 1]);
}

// ---------------------------------------------------------------- Edit / Pause / Step

BOOST_AUTO_TEST_CASE(Frozen_EditAndPauseDoNotExchangeStepDoesExactlyOne)
{
	auto set = make_set();
	auto& a = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		reg.ctx().get<ecs::blackboard_writer>().set(k_frame, frame);
		reg.ctx().get<ecs::outbox<msg>>().send({ "a" }, msg{ frame });
	});

	set->set_mode(ecs::run_mode::edit);
	set->channel().post_external<msg>({ "a" }, msg{ 100 });
	for (int i = 0; i < 3; ++i) set->update(one_frame);
	BOOST_TEST(set->channel().exchange_count() == 0u);
	BOOST_TEST(set->channel().reader().find(k_frame) == nullptr);
	for (const auto& v : seen(a).inbox_values) BOOST_TEST(v.empty());
	BOOST_TEST(a.state().ctx().get<ecs::outbox<msg>>().pending().size() == 3u); // accumulates

	set->set_mode(ecs::run_mode::play);
	set->update(one_frame);
	BOOST_TEST(set->channel().exchange_count() == 1u);
	BOOST_TEST((seen(a).inbox_values.back() == std::vector<int>{ 100, 0, 1, 2 }));
	BOOST_TEST(*set->channel().reader().find(k_frame) == 2);

	set->set_mode(ecs::run_mode::pause);
	for (int i = 0; i < 2; ++i) set->update(one_frame);
	BOOST_TEST(set->channel().exchange_count() == 1u);
	BOOST_TEST(seen(a).inbox_values.back().empty()); // consumed, never seen twice
	BOOST_TEST(*set->channel().reader().find(k_frame) == 2);

	set->set_mode(ecs::run_mode::step);
	set->update(one_frame);
	BOOST_TEST(set->channel().exchange_count() == 2u);
	BOOST_TEST((seen(a).inbox_values.back() == std::vector<int>{ 3, 4, 5 }));
	BOOST_TEST((set->mode() == ecs::run_mode::pause));
	set->update(one_frame);
	BOOST_TEST(set->channel().exchange_count() == 2u);
}

// ---------------------------------------------------------------- order independence, repeatability

BOOST_AUTO_TEST_CASE(Determinism_ThePhaseOrderOfTheContextsDoesNotChangeTheResult)
{
	const auto a_first = run_pingpong(true);
	const auto a_last = run_pingpong(false);

	BOOST_REQUIRE(!a_first.empty());
	BOOST_TEST((a_first == a_last));
}

BOOST_AUTO_TEST_CASE(Determinism_TwoIdenticalRunsAreEqual)
{
	log_capture quiet; // the scenario has a writer conflict on purpose
	const auto first = run_pingpong(true);
	const auto second = run_pingpong(true);

	BOOST_TEST((first == second));
	// and the scenario really exchanged something
	bool saw_message = false;
	for (const auto& line : first) saw_message = saw_message || line.find("[b>a#") != std::string::npos;
	BOOST_TEST(saw_message);
}

// ---------------------------------------------------------------- lifetime

BOOST_AUTO_TEST_CASE(Lifetime_TheBlackboardSurvivesARecreatedContextItsInboxDoesNot)
{
	auto set = make_set();
	add(*set, "keep", 0, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame == 0) reg.ctx().get<ecs::blackboard_writer>().set(k_shared, 8);
	});
	add(*set, "worker", 1, { "script_early" }, [](entt::registry& reg, int frame) {
		record(reg);
		if (frame == 0) reg.ctx().get<ecs::outbox<msg>>().send({ "worker" }, msg{ 3 });
	});

	set->update(one_frame);
	set->update(one_frame); // the worker has its message in the inbox now
	BOOST_TEST(set->get("worker").state().ctx().get<ecs::inbox<msg>>().messages().size() == 1u);

	// what reload_context does: destroy and create under the same name and id
	set->destroy_context("worker");
	auto& again = add(*set, "worker", 1, { "script_early" }, [](entt::registry& reg, int) { record(reg); });
	set->update(one_frame);

	BOOST_TEST(again.state().ctx().get<ecs::inbox<msg>>().messages().empty());
	BOOST_TEST(*set->channel().reader().find(k_shared) == 8);
	BOOST_TEST(seen(again).shared_seen.back() == 8);
}

BOOST_AUTO_TEST_CASE(Lifetime_ClearForgetsTheBlackboardAndUndeliveredPosts)
{
	auto set = make_set();
	add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int) { record(reg); });
	set->channel().external_blackboard().set(k_shared, 1);
	set->update(one_frame);
	set->channel().post_external<msg>({}, msg{ 1 });
	BOOST_TEST(*set->channel().reader().find(k_shared) == 1);

	set->clear();
	auto& fresh = add(*set, "a", 0, { "script_early" }, [](entt::registry& reg, int) { record(reg); });
	set->update(one_frame);

	BOOST_TEST(set->channel().reader().find(k_shared) == nullptr);
	BOOST_TEST(fresh.state().ctx().get<ecs::inbox<msg>>().messages().empty());
}

// ---------------------------------------------------------------- isolation, external entry

BOOST_AUTO_TEST_CASE(Isolation_ASetNeverSeesTheChannelOfAnotherSet)
{
	mgr.create_set("preview:p");
	mgr.create_set("play");
	auto script_body = [](entt::registry& reg, int) { record(reg); };
	for (const char* set : { "level", "preview:p", "play" }) {
		auto& context = mgr.create_context(set, "w", scn::context_kind::runtime, { "script_early" });
		context.state().ctx().emplace<script>(script{ script_body, 0 });
	}

	mgr.post("level", {}, msg{ 1 });
	mgr.post("level", { "w" }, msg{ 2 });
	mgr.blackboard("level").set(k_shared, 10);
	mgr.post("play", {}, msg{ 3 });
	mgr.blackboard("preview:p").set(k_shared, 30);
	mgr.update(one_frame);

	BOOST_TEST((seen(*mgr.find("level", "w")).inbox_values[0] == std::vector<int>{ 1, 2 }));
	BOOST_TEST((seen(*mgr.find("play", "w")).inbox_values[0] == std::vector<int>{ 3 }));
	BOOST_TEST(seen(*mgr.find("preview:p", "w")).inbox_values[0].empty());

	mgr.update(one_frame); // the values stay where they were written
	BOOST_TEST(seen(*mgr.find("level", "w")).shared_seen[1] == 10);
	BOOST_TEST(seen(*mgr.find("play", "w")).shared_seen[1] == -1);
	BOOST_TEST(seen(*mgr.find("preview:p", "w")).shared_seen[1] == 30);
	BOOST_TEST(seen(*mgr.find("level", "w")).shared_seen[0] == 10);
	BOOST_TEST(seen(*mgr.find("play", "w")).shared_seen[0] == -1);
	BOOST_TEST(seen(*mgr.find("preview:p", "w")).shared_seen[0] == 30);
}

BOOST_AUTO_TEST_CASE(Isolation_AnInactiveSetKeepsItsChannelFrozen)
{
	mgr.create_set("play");
	auto& context = mgr.create_context("play", "w", scn::context_kind::play, { "script_early" });
	context.state().ctx().emplace<script>(script{ [](entt::registry& reg, int) { record(reg); }, 0 });
	mgr.get_set("play").set_active(false);

	mgr.post("play", {}, msg{ 1 });
	mgr.update(one_frame);
	mgr.update(one_frame);
	BOOST_TEST(mgr.get_set("play").channel().exchange_count() == 0u);

	mgr.get_set("play").set_active(true);
	mgr.update(one_frame);
	BOOST_TEST((seen(context).inbox_values[0] == std::vector<int>{ 1 }));
}

// ---------------------------------------------------------------- system access

BOOST_AUTO_TEST_CASE(Access_ChannelResourcesAreTrackedAsOrdinaryCtxResources)
{
	using args = std::tuple<const ecs::blackboard_reader&, ecs::blackboard_writer&, const ecs::inbox<msg>&, ecs::outbox<msg>&>;
	const auto access = ecs::collect_access(static_cast<args*>(nullptr));

	BOOST_REQUIRE(access.size() == 4u);
	BOOST_TEST(!access[0].write);
	BOOST_TEST(access[1].write);
	BOOST_TEST(!access[2].write);
	BOOST_TEST(access[3].write);
}

BOOST_AUTO_TEST_SUITE_END()
