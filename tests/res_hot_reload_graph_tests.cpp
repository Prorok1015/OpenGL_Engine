#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

// A change of a resource reaches the ones built from it along the reverse edges of the dependency graph (US-17-5).
// Manual task_system: nothing loads or is delivered until the test pumps, so the order of the calls is the order of
// the queue.

namespace {

// A node is its label, followed by the value of each dependency in brackets: "g(c(p1))".
// Its raw data is the label on the first line and one dependency tag per line after it.
struct node_resource : res::text_resource
{
	node_resource(const res::tag& tag, std::string text) : res::text_resource(tag, {})
	{
		content = std::move(text);
	}
};

std::vector<std::string> lines_of(const std::vector<std::byte>& raw)
{
	std::vector<std::string> lines;
	std::istringstream stream(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
	for (std::string line; std::getline(stream, line);) {
		if (!line.empty()) lines.push_back(line);
	}
	return lines;
}

struct node_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<node_resource>({ "node" });
	static inline std::atomic<int> loads{ 0 };

	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>& raw) const override
	{
		std::vector<res::res_dep> deps;
		const auto lines = lines_of(raw);
		for (std::size_t i = 1; i < lines.size(); ++i) {
			deps.push_back(res::make_dep<node_resource>(res::tag{ lines[i] }));
		}
		return deps;
	}

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps) const override
	{
		++loads;
		const auto lines = lines_of(raw);
		std::string text = lines.empty() ? std::string{} : lines.front();
		if (!deps.empty()) {
			text += "(";
			for (std::size_t i = 0; i < deps.size(); ++i) {
				if (i) text += ",";
				text += deps.get<node_resource>(deps.entries()[i].dep.target)->str();
			}
			text += ")";
		}
		return std::make_shared<node_resource>(tag, std::move(text));
	}

	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

// One adapter for the base type serves the derived type too.
struct shared_base : res::text_resource
{
	using res::text_resource::text_resource;
};

struct shared_derived : shared_base
{
	using base_type = shared_base;
	using shared_base::shared_base;
};

struct shared_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<shared_base>({ "shared" });
	static inline std::atomic<int> loads{ 0 };

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		++loads;
		return std::make_shared<shared_derived>(tag, raw);
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

struct graph_env : test_util::ram_res_env
{
	graph_env()
	{
		rs->registrate_adapter<node_adapter>(node_adapter::INFO);
		rs->registrate_adapter<shared_adapter>(shared_adapter::INFO);
		node_adapter::loads = 0;
		shared_adapter::loads = 0;
	}

	// Writes the node; its dependencies are the tags in `needs`.
	res::tag node(std::string_view name, std::string_view label, const std::vector<res::tag>& needs = {})
	{
		std::string body{ label };
		for (const auto& need : needs) {
			body += "\n" + need.string();
		}
		return put(name, body);
	}

	void watch(const std::vector<res::tag>& tags)
	{
		for (const auto& tag : tags) {
			owners.push_back(std::make_unique<int>(0));
			rs->watch(tag, owners.back().get(), [this](const res::tag& changed) { delivered.push_back(changed.string()); });
		}
	}

	int times_delivered(const res::tag& tag) const
	{
		return static_cast<int>(std::count(delivered.begin(), delivered.end(), tag.string()));
	}

	std::ptrdiff_t position_of(const res::tag& tag) const
	{
		return std::find(delivered.begin(), delivered.end(), tag.string()) - delivered.begin();
	}

	std::vector<std::string> delivered;
	std::vector<std::unique_ptr<int>> owners;
};

}

BOOST_FIXTURE_TEST_SUITE(ResHotReloadGraphTests, graph_env)

BOOST_AUTO_TEST_CASE(ParentChange_ReloadsChildAndGrandchildOnce_ParentsFirst)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	const auto g = node("g.node", "g", { c });
	auto first = rs->require<node_resource>(g);
	pump();
	BOOST_REQUIRE(first.is_ready());
	BOOST_TEST(first.get()->str() == "g(c(p1))");
	BOOST_TEST(node_adapter::loads.load() == 3);

	watch({ p, c, g });
	node("p.node", "p2");
	rs->signal_changed(p);
	BOOST_TEST(delivered.empty()); // watchers are the main thread's, and it has not drained yet
	pump();

	const std::vector<std::string> expected{ p.string(), c.string(), g.string() };
	BOOST_CHECK_EQUAL_COLLECTIONS(delivered.begin(), delivered.end(), expected.begin(), expected.end());

	auto second = rs->require<node_resource>(g);
	pump();
	BOOST_TEST(second.get_control_block() != first.get_control_block());
	BOOST_TEST(second.get()->str() == "g(c(p2))");
	BOOST_TEST(first.get()->str() == "g(c(p1))"); // the holder of the old handle is not invalidated
	BOOST_TEST(node_adapter::loads.load() == 6);  // each of the three loaded once more
}

BOOST_AUTO_TEST_CASE(Diamond_ReloadsTheSharedChildOnce)
{
	const auto base = node("base.node", "base1");
	const auto left = node("left.node", "l", { base });
	const auto right = node("right.node", "r", { base });
	const auto top = node("top.node", "top", { left, right });
	auto first = rs->require<node_resource>(top);
	pump();
	BOOST_REQUIRE(first.is_ready());
	BOOST_TEST(first.get()->str() == "top(l(base1),r(base1))");
	BOOST_TEST(node_adapter::loads.load() == 4);

	watch({ base, left, right, top });
	node("base.node", "base2");
	rs->signal_changed(base);
	pump();

	BOOST_TEST(delivered.size() == 4u);
	for (const auto& tag : { base, left, right, top }) {
		BOOST_TEST(times_delivered(tag) == 1);
	}
	BOOST_TEST(position_of(base) < position_of(left));
	BOOST_TEST(position_of(base) < position_of(right));
	BOOST_TEST(position_of(left) < position_of(top));
	BOOST_TEST(position_of(right) < position_of(top));

	auto second = rs->require<node_resource>(top);
	pump();
	BOOST_TEST(second.get()->str() == "top(l(base2),r(base2))");
	BOOST_TEST(node_adapter::loads.load() == 8); // the shared base was loaded once for both of its dependents
}

BOOST_AUTO_TEST_CASE(UnrelatedResources_AreLeftAlone)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	const auto other = node("other.node", "o");
	const auto sibling = node("sibling.node", "s", { other });
	auto held_other = rs->require<node_resource>(other);
	auto held_sibling = rs->require<node_resource>(sibling);
	auto held_child = rs->require<node_resource>(c);
	pump();

	watch({ other, sibling });
	node("p.node", "p2");
	rs->signal_changed(p);
	pump();

	BOOST_TEST(delivered.empty());
	BOOST_TEST(rs->require<node_resource>(other).get_control_block() == held_other.get_control_block());
	BOOST_TEST(rs->require<node_resource>(sibling).get_control_block() == held_sibling.get_control_block());
	BOOST_TEST(rs->require<node_resource>(c).get_control_block() != held_child.get_control_block());
}

BOOST_AUTO_TEST_CASE(Cycle_DoesNotLoopAndEachMemberIsDeliveredOnce)
{
	const auto a = tag_of("a.node");
	const auto b = tag_of("b.node");
	node("a.node", "a", { b });
	node("b.node", "b", { a });
	auto a_handle = rs->require<node_resource>(a);
	auto b_handle = rs->require<node_resource>(b);
	pump();
	BOOST_REQUIRE(a_handle.has_error());
	BOOST_REQUIRE(b_handle.has_error());

	watch({ a, b });
	rs->signal_changed(a);
	pump();
	BOOST_TEST(delivered.size() == 2u);
	BOOST_TEST(times_delivered(a) == 1);
	BOOST_TEST(times_delivered(b) == 1);

	// the cycle is broken in the files: the tags load again
	node("b.node", "b");
	delivered.clear();
	rs->signal_changed(b);
	pump();
	BOOST_TEST(delivered.size() == 2u);
	auto again = rs->require<node_resource>(a);
	pump();
	BOOST_TEST(again.is_ready());
	BOOST_TEST(again.get()->str() == "a(b)");
}

BOOST_AUTO_TEST_CASE(ChildLoadInFlight_IsEvictedAndLoadsAgainFromTheNewParent)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	auto stale = rs->require<node_resource>(c);
	BOOST_REQUIRE(tasks.run_one(ds::lane::compute)); // c collected its dependencies and asked for p, which has not loaded
	BOOST_REQUIRE(!stale.is_ready());

	watch({ p, c });
	node("p.node", "p2");
	rs->signal_changed(p);
	auto fresh = rs->require<node_resource>(c);
	BOOST_TEST(fresh.get_control_block() != stale.get_control_block());
	pump();

	BOOST_REQUIRE(fresh.is_ready());
	BOOST_TEST(fresh.get()->str() == "c(p2)");
	BOOST_REQUIRE(stale.is_ready()); // it finishes on its own with the parent it was started with, and nobody is waiting
	BOOST_TEST(stale.get()->str() == "c(p1)");
	BOOST_TEST(times_delivered(p) == 1);
	BOOST_TEST(times_delivered(c) == 1);
	BOOST_TEST(position_of(p) < position_of(c));
}

BOOST_AUTO_TEST_CASE(ChildQueuedButNotYetCollected_PicksUpTheNewParentByItself)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	auto child = rs->require<node_resource>(c); // nothing ran: no edge to c exists yet

	watch({ p, c });
	node("p.node", "p2");
	rs->signal_changed(p);
	pump();

	BOOST_REQUIRE(child.is_ready());
	BOOST_TEST(child.get()->str() == "c(p2)");
	BOOST_TEST(rs->require<node_resource>(c).get_control_block() == child.get_control_block());
	BOOST_TEST(times_delivered(p) == 1);
	BOOST_TEST(times_delivered(c) == 0);
}

BOOST_AUTO_TEST_CASE(ChildThatNoLongerNeedsTheParent_IsNotFollowedAnyMore)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	auto first = rs->require<node_resource>(c);
	pump();
	BOOST_REQUIRE(first.is_ready());

	node("c.node", "c"); // the file dropped its dependency
	rs->signal_changed(c);
	auto second = rs->require<node_resource>(c);
	pump();
	BOOST_REQUIRE(second.is_ready());
	BOOST_TEST(second.get()->str() == "c");

	watch({ p, c });
	rs->signal_changed(p);
	pump();
	BOOST_TEST(delivered.size() == 1u);
	BOOST_TEST(times_delivered(p) == 1);
	BOOST_TEST(rs->require<node_resource>(c).get_control_block() == second.get_control_block());
}

BOOST_AUTO_TEST_CASE(SignalsOfOneDrain_DeliverEachTagOnceAndParentsFirst)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	auto held = rs->require<node_resource>(c);
	pump();
	watch({ p, c });

	rs->signal_changed(c); // the child's own file changed together with the parent's
	rs->signal_changed(p);
	BOOST_TEST(tasks.drain_main() == 1u);

	const std::vector<std::string> expected{ p.string(), c.string() };
	BOOST_CHECK_EQUAL_COLLECTIONS(delivered.begin(), delivered.end(), expected.begin(), expected.end());
	pump();
}

BOOST_AUTO_TEST_CASE(DependentWithoutWatchers_IsStillEvicted)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	auto held = rs->require<node_resource>(c);
	pump();

	rs->signal_changed(p);
	pump();
	BOOST_TEST(rs->require<node_resource>(c).get_control_block() != held.get_control_block());
	pump();
}

BOOST_AUTO_TEST_CASE(OneCacheBlockServesTheBaseTypeAndTheTypeDerivedFromIt)
{
	const auto tag = put("one.shared", "payload");
	auto as_base = rs->require<shared_base>(tag);
	auto as_derived = rs->require<shared_derived>(tag);
	pump();

	BOOST_TEST(as_base.get_control_block() == as_derived.get_control_block());
	BOOST_TEST(shared_adapter::loads.load() == 1);
	BOOST_REQUIRE(as_derived.is_ready());
	BOOST_TEST(as_derived.get()->str() == "payload");
}

BOOST_AUTO_TEST_SUITE_END()
