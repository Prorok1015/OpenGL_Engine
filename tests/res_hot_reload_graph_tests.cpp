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

// Holds the data of the requests it is told to, until release() hands it over: a load whose raw data arrives late.
class gated_resolver : public test_util::ram_resolver
{
public:
	explicit gated_resolver(const std::string_view prt) : ram_resolver(prt) {}

	async_raw_data resolve(const res::tag& tag) const override
	{
		if (!gate) {
			return ram_resolver::resolve(tag);
		}
		auto block = std::make_shared<core::res::res_control_block<std::vector<std::byte>>>();
		held.emplace_back(block, files.at(tag));
		return block;
	}

	void release()
	{
		for (auto& [block, data] : held) {
			block->set_ready(std::move(data));
		}
		held.clear();
	}

	bool gate = false;
	mutable std::vector<std::pair<std::shared_ptr<core::res::res_control_block<std::vector<std::byte>>>, std::vector<std::byte>>> held;
};

struct graph_env : test_util::ram_res_env
{
	~graph_env()
	{
		// A task left in the queue must not call into the members below.
		for (const auto& [tag, owner] : watched) {
			rs->unwatch(tag, owner);
		}
	}

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
			watched.emplace_back(tag, owners.back().get());
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
	std::vector<std::pair<res::tag, void*>> watched;
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

BOOST_AUTO_TEST_CASE(EachSignalDeliversItsOwnBatchInOneMainTask)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	auto held = rs->require<node_resource>(c);
	pump();
	watch({ p, c });

	rs->signal_changed(p);
	rs->signal_changed(p);
	BOOST_TEST(tasks.drain_main() == 2u);

	const std::vector<std::string> expected{ p.string(), c.string(), p.string(), c.string() };
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

// Load #1 of c is evicted, load #2 replaces it, and then #1 gets to record: the edges stay those of #2.
BOOST_AUTO_TEST_CASE(EvictedLoadThatRecordsLate_DoesNotOverwriteTheEdgesOfItsReplacement)
{
	rs->unregistrate_resolver(res::tag::memory);
	auto& gated = rs->registrate_resolver<gated_resolver>(res::tag::memory);
	const auto d = node("d.node", "d");
	const auto e = node("e.node", "e");
	const auto c = node("c.node", "c", { d });

	gated.gate = true;
	auto first = rs->require<node_resource>(c); // its raw data is held back
	node("c.node", "c", { e });
	rs->signal_changed(c);
	gated.gate = false;
	auto second = rs->require<node_resource>(c);
	pump();
	BOOST_REQUIRE(second.is_ready());
	BOOST_TEST(second.get()->str() == "c(e)");

	gated.release(); // #1 collects only now, after #2 recorded {e}
	pump();

	watch({ d, e, c });
	delivered.clear();
	rs->signal_changed(d);
	pump();
	BOOST_TEST(times_delivered(c) == 0);
	rs->signal_changed(e);
	pump();
	BOOST_TEST(times_delivered(c) == 1);
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

// The edges of a tag nobody holds any more are stale: the tag is not loaded, and a change of what it was built from
// must not evict or deliver it (#311).
BOOST_AUTO_TEST_CASE(ReleasedDependent_IsNotEvictedNorDeliveredAnyMore)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	{
		auto loaded = rs->require<node_resource>(c);
		pump();
		BOOST_REQUIRE(loaded.is_ready());
	} // released: the cache keeps only an expired entry of c

	watch({ p, c });
	const int loads_before = node_adapter::loads.load();
	rs->signal_changed(p);
	pump();

	BOOST_TEST(delivered.size() == 1u);
	BOOST_TEST(times_delivered(p) == 1);
	BOOST_TEST(times_delivered(c) == 0);

	auto again = rs->require<node_resource>(c); // c loads again on demand, and its edges come back
	pump();
	BOOST_TEST(node_adapter::loads.load() == loads_before + 2); // c, and p which the signal evicted
	delivered.clear();
	rs->signal_changed(p);
	pump();
	BOOST_TEST(times_delivered(c) == 1);
}

// g is held, c (between g and p) is not: a change of p still has to reach g through the released c, whose watcher
// is part of the chain as well.
BOOST_AUTO_TEST_CASE(ReleasedMiddleOfAChain_StillPassesTheChangeOn)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	const auto g = node("g.node", "g", { c });
	auto held = rs->require<node_resource>(g);
	pump();
	BOOST_REQUIRE(held.is_ready());
	BOOST_REQUIRE(held.get()->str() == "g(c(p1))");

	watch({ p, c, g });
	rs->signal_changed(p);
	pump();

	BOOST_TEST(times_delivered(p) == 1);
	BOOST_TEST(times_delivered(g) == 1);
	BOOST_TEST(times_delivered(c) == 1);
	BOOST_TEST(position_of(p) < position_of(c));
	BOOST_TEST(position_of(c) < position_of(g));

	// c itself changing still reaches g
	delivered.clear();
	rs->signal_changed(c);
	pump();
	BOOST_TEST(times_delivered(g) == 1);
}

BOOST_AUTO_TEST_CASE(ReleasedResources_DoNotMakeTheGraphGrowWithoutBound)
{
	const auto p = node("p.node", "p1");
	constexpr int count = 1000;
	for (int i = 0; i < count; ++i) {
		const auto tag = node("n" + std::to_string(i) + ".node", "n", { p });
		auto loaded = rs->require<node_resource>(tag);
		pump();
		BOOST_REQUIRE(loaded.is_ready());
	}
	BOOST_TEST(rs->dependency_edge_count() < static_cast<std::size_t>(count / 2));
}

// A signal evicts c and leaves its edge behind; c is not loaded again. The next sweep drops it (#326).
BOOST_AUTO_TEST_CASE(EvictedAndNeverReloaded_LosesItsEdgesAtTheNextSweep)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	const auto other = node("q.node", "q");
	auto held = rs->require<node_resource>(c);
	pump();
	BOOST_REQUIRE(held.is_ready());
	BOOST_REQUIRE(rs->dependency_edge_count() == 1u);

	rs->signal_changed(p); // c is evicted, its handle is stale
	pump();
	BOOST_TEST(rs->dependency_edge_count() == 1u); // the sweep of that signal ran before the eviction

	rs->signal_changed(other);
	pump();
	BOOST_TEST(rs->dependency_edge_count() == 0u);
}

// A watcher is told about the next change of an evicted tag whether or not the tag was loaded again, so the edges
// stay while the tag is watched and go once nobody watches it.
BOOST_AUTO_TEST_CASE(EvictedButWatched_KeepsItsEdgesUntilUnwatched)
{
	const auto p = node("p.node", "p1");
	const auto c = node("c.node", "c", { p });
	const auto other = node("q.node", "q");
	auto held = rs->require<node_resource>(c);
	pump();
	watch({ c });

	rs->signal_changed(p);
	rs->signal_changed(other);
	pump();
	BOOST_TEST(rs->dependency_edge_count() == 1u);

	rs->unwatch(c, owners.front().get());
	rs->signal_changed(other);
	pump();
	BOOST_TEST(rs->dependency_edge_count() == 0u);
	watched.clear(); // the fixture would unwatch it a second time
}

// The block is cached under the alias target, the watcher may have been put on the original tag: the edges stay.
BOOST_AUTO_TEST_CASE(EvictedAndWatchedThroughAnAlias_KeepsItsEdges)
{
	const auto p = node("p.node", "p1");
	const auto original = node("orig.node", "orig");
	const auto generated = node("gen.node", "gen", { p });
	const auto other = node("q.node", "q");
	rs->register_alias(original, generated);
	auto held = rs->require<node_resource>(original);
	pump();
	BOOST_REQUIRE(held.is_ready());
	BOOST_REQUIRE(rs->dependency_edge_count() == 1u);
	watch({ original });

	rs->signal_changed(p);
	rs->signal_changed(other);
	pump();
	BOOST_TEST(rs->dependency_edge_count() == 1u);
}

// The tags still in use keep their edges while the evicted ones lose theirs.
BOOST_AUTO_TEST_CASE(LiveDependent_KeepsItsEdgesWhenEvictedOnesAreDropped)
{
	const auto p = node("p.node", "p1");
	const auto p2 = node("p2.node", "p2");
	const auto gone = node("gone.node", "gone", { p });
	const auto live = node("live.node", "live", { p2 });
	const auto other = node("q.node", "q");
	auto held_gone = rs->require<node_resource>(gone);
	auto held_live = rs->require<node_resource>(live);
	pump();
	BOOST_REQUIRE(held_live.is_ready());
	BOOST_REQUIRE(rs->dependency_edge_count() == 2u);

	rs->signal_changed(p);
	rs->signal_changed(other);
	pump();
	BOOST_TEST(rs->dependency_edge_count() == 1u);

	watch({ live });
	rs->signal_changed(p2);
	pump();
	BOOST_TEST(times_delivered(live) == 1);
}

// Above the floor a burst of signals sweeps once per k_signal_sweep_every, not once per signal.
BOOST_AUTO_TEST_CASE(SignalSweeps_AreThrottledOnALargeGraph)
{
	const auto p = node("p.node", "p1");
	const auto other = node("q.node", "q");
	std::vector<res::res_handle<node_resource>> held;
	for (int i = 0; i < 300; ++i) {
		held.push_back(rs->require<node_resource>(node("n" + std::to_string(i) + ".node", "n", { p })));
		pump();
		BOOST_REQUIRE(held.back().is_ready());
	}
	BOOST_REQUIRE(rs->dependency_edge_count() == 300u);

	const auto before = rs->edge_sweep_count();
	constexpr int signals = 32;
	for (int i = 0; i < signals; ++i) {
		rs->signal_changed(other);
	}
	pump();
	const auto sweeps = rs->edge_sweep_count() - before;
	BOOST_TEST(sweeps >= 1u);
	BOOST_TEST(sweeps <= 2u);
	BOOST_TEST(rs->dependency_edge_count() == 300u); // all held: nothing to drop
}

// original -> alias -> c: a change of the original reaches c, built from the alias, through the alias edge (#302).
BOOST_AUTO_TEST_CASE(AliasEdge_PassesTheChangeOnToWhatDependsOnTheAlias)
{
	const auto original = node("orig.node", "o");
	const auto alias = node("alias.node", "a1");
	const auto c = node("c.node", "c", { alias });
	rs->register_alias(original, alias);
	auto held = rs->require<node_resource>(c);
	pump();
	BOOST_REQUIRE(held.is_ready());
	watch({ alias, c });

	rs->signal_changed(original);
	pump();

	BOOST_TEST(times_delivered(alias) == 1);
	BOOST_TEST(times_delivered(c) == 1);
	BOOST_TEST(position_of(alias) < position_of(c));
	auto again = rs->require<node_resource>(c);
	pump();
	BOOST_TEST(again.get_control_block() != held.get_control_block());
	BOOST_TEST(rs->resolve_alias(original) == original); // dropped until the next load registers it
}

// Two aliases pointing at each other: the signal follows the loop once.
BOOST_AUTO_TEST_CASE(AliasEdge_Cycle_DeliversEachTagOnce)
{
	const auto a = node("a.node", "a");
	const auto b = node("b.node", "b");
	rs->register_alias(a, b);
	rs->register_alias(b, a);
	watch({ a, b });

	rs->signal_changed(a);
	pump();

	BOOST_TEST(times_delivered(a) == 1);
	BOOST_TEST(times_delivered(b) == 1);
}

BOOST_AUTO_TEST_SUITE_END()
