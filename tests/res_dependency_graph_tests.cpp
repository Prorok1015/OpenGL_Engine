#include <boost/test/unit_test.hpp>
#include "test_scene_env.hpp"
#include <snk/resource/adapters/res_text_adapter.h>
#include <snk/scene/adapters/scn_worldwrap_adapter.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_text.h>
#include <atomic>
#include <sstream>
#include <string>
#include <vector>

// The resource dependency pipeline and its wait graph on a manual task_system: nothing runs until the test pumps,
// so the order of the queue is the order of the calls.

namespace {

struct counted_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

// Its raw data lists the resources it needs, one tag per line. The kind of each follows from its extension:
// .cnt is counted_resource, .join is joined_resource, anything else is text. Its value is their values joined by '+'.
struct joined_resource : res::text_resource
{
	using res::text_resource::text_resource;
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

struct counted_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<counted_resource>({ "cnt" });
	static inline std::atomic<int> loads{ 0 };

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw) const override
	{
		++loads;
		return std::make_shared<counted_resource>(tag, raw);
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

struct joined_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<joined_resource>({ "join" });
	static inline std::atomic<int> collects{ 0 };
	static inline std::atomic<int> deserializes{ 0 };

	static std::string text_of(const res::res_deps& deps, const res::res_dep& dep)
	{
		const auto ext = dep.target.extension();
		for (const auto& item : deps.entries()) {
			if (item.dep.target == dep.target) {
				if (ext == "cnt") return deps.get<counted_resource>(dep.target)->str();
				if (ext == "join") return deps.get<joined_resource>(dep.target)->str();
				return deps.get<res::text_resource>(dep.target)->str();
			}
		}
		return "?";
	}

	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>& raw) const override
	{
		++collects;
		std::vector<res::res_dep> deps;
		for (const auto& line : lines_of(raw)) {
			const res::tag target{ line };
			if (target.extension() == "cnt") deps.push_back(res::make_dep<counted_resource>(target));
			else if (target.extension() == "join") deps.push_back(res::make_dep<joined_resource>(target));
			else deps.push_back(res::make_dep<res::text_resource>(target));
		}
		return deps;
	}

	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps& deps) const override
	{
		++deserializes;
		std::string joined;
		const auto declared = collect_dependencies(tag, raw);
		BOOST_REQUIRE_EQUAL(deps.size(), declared.size());
		for (std::size_t i = 0; i < declared.size(); ++i) {
			BOOST_TEST((deps.entries()[i].dep.target == declared[i].target)); // in the order they were declared
			joined += (i ? "+" : "") + text_of(deps, declared[i]);
		}
		return std::make_shared<joined_resource>(tag, test_util::to_bytes(joined));
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

// Needs the resource its raw data names, of its own kind: a cycle when the names lead back.
struct loop_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

struct loop_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<loop_resource>({ "loop" });

	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>& raw) const override
	{
		std::vector<res::res_dep> deps;
		for (const auto& line : lines_of(raw)) {
			deps.push_back(res::make_dep<loop_resource>(res::tag{ line }));
		}
		return deps;
	}
	std::shared_ptr<res::resource_entry> deserialize(const res::tag& tag, const std::vector<std::byte>& raw, const res::res_deps&) const override
	{
		return std::make_shared<loop_resource>(tag, raw);
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

struct throwing_resource : res::text_resource
{
	using res::text_resource::text_resource;
};

struct throwing_adapter : core::res::adapter_interface
{
	static inline auto INFO = res::adapter_info::make<throwing_resource>({ "throw" });

	std::vector<res::res_dep> collect_dependencies(const res::tag&, const std::vector<std::byte>&) const override
	{
		throw std::runtime_error("cannot tell what this needs");
	}
	std::vector<std::byte> serialize(const res::tag&, const std::shared_ptr<res::resource_entry>&) const override { return {}; }
};

bool mentions(const std::shared_ptr<core::res::res_resource_block>& block, std::string_view text)
{
	return block->error_msg.find(text) != std::string::npos;
}

struct dep_env : test_util::scene_env
{
	dep_env()
	{
		rs->registrate_adapter<counted_adapter>(counted_adapter::INFO);
		rs->registrate_adapter<joined_adapter>(joined_adapter::INFO);
		rs->registrate_adapter<loop_adapter>(loop_adapter::INFO);
		rs->registrate_adapter<throwing_adapter>(throwing_adapter::INFO);
		counted_adapter::loads = 0;
		joined_adapter::collects = 0;
		joined_adapter::deserializes = 0;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ResDependencyGraphTests, dep_env)

// The original deadlock: C needs P, and P's load is already queued behind C's.
BOOST_AUTO_TEST_CASE(ChildQueuedBeforeItsParent_BothLoad)
{
	const auto parent = put("parent.txt", "parent data");
	const auto child = put("child.join", parent.string());

	auto child_handle = rs->require<joined_resource>(child);
	auto parent_handle = rs->require<res::text_resource>(parent);
	pump();

	BOOST_REQUIRE(child_handle.is_ready());
	BOOST_TEST(child_handle.get()->str() == "parent data");
	BOOST_TEST(parent_handle.is_ready());
}

BOOST_AUTO_TEST_CASE(TextAdapter_HasNoDependenciesByDefault)
{
	res::text_adapter adapter;
	BOOST_TEST(adapter.collect_dependencies(tag_of("a.txt"), test_util::to_bytes("x")).empty());
}

BOOST_AUTO_TEST_CASE(DeserializeGetsTheDependenciesInTheOrderTheyWereDeclared)
{
	const auto one = put("one.txt", "1");
	const auto two = put("two.cnt", "2");
	const auto three = put("three.txt", "3");
	const auto all = put("all.join", one.string() + "\n" + two.string() + "\n" + three.string());

	auto handle = rs->require<joined_resource>(all);
	pump();

	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get()->str() == "1+2+3");
}

BOOST_AUTO_TEST_CASE(NothingRunsBeforeTheDependenciesAreReady)
{
	const auto leaf = put("leaf.txt", "leaf");
	const auto root = put("root.join", leaf.string());
	auto handle = rs->require<joined_resource>(root);

	BOOST_TEST(tasks.run_one(ds::lane::compute)); // collect: the dependency is requested, nothing waits
	BOOST_TEST(joined_adapter::collects.load() == 1);
	BOOST_TEST(joined_adapter::deserializes.load() == 0);
	BOOST_TEST(!handle.is_ready());

	BOOST_TEST(tasks.run_one(ds::lane::compute)); // the dependency itself
	BOOST_TEST(tasks.run_one(ds::lane::compute)); // deserialize, queued once the dependency was ready
	BOOST_TEST(!tasks.run_one(ds::lane::compute));
	BOOST_TEST(handle.is_ready());
	BOOST_TEST(joined_adapter::deserializes.load() == 1);
}

BOOST_AUTO_TEST_CASE(DiamondDependency_LoadsTheSharedResourceOnce)
{
	const auto shared = put("shared.cnt", "z");
	const auto left = put("left.join", shared.string());
	const auto right = put("right.join", shared.string());
	const auto top = put("top.join", left.string() + "\n" + right.string());

	auto handle = rs->require<joined_resource>(top);
	pump();

	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get()->str() == "z+z");
	BOOST_TEST(counted_adapter::loads.load() == 1);
}

BOOST_AUTO_TEST_CASE(FailedDependency_FailsTheDependentAndNamesIt)
{
	const auto root = put("root.join", "memory://t/absent.txt");
	auto handle = rs->require<joined_resource>(root);
	pump();

	BOOST_TEST(handle.has_error());
	BOOST_TEST(mentions(handle.get_control_block(), "Dependency failed"));
	BOOST_TEST(mentions(handle.get_control_block(), "absent.txt"));
}

BOOST_AUTO_TEST_CASE(CollectDependenciesThrowing_FailsTheResource)
{
	const auto tag = put("bad.throw", "x");
	auto handle = rs->require<throwing_resource>(tag);
	pump();

	BOOST_TEST(handle.has_error());
	BOOST_TEST(mentions(handle.get_control_block(), "cannot tell what this needs"));
}

BOOST_AUTO_TEST_CASE(RequireSync_OfABlockThatIsStillPending_WaitsForIt)
{
	const auto tag = put("pending.txt", "p");
	auto first = rs->require<res::text_resource>(tag); // queued, nothing has run
	BOOST_TEST(!first.is_ready());

	auto second = rs->require_sync<res::text_resource>(tag);
	BOOST_TEST(second.is_ready());
	BOOST_TEST(second.get_control_block() == first.get_control_block());
}

BOOST_AUTO_TEST_CASE(RequireSync_OfAResourceWithDependencies_ReturnsItLoaded)
{
	const auto leaf = put("leaf.cnt", "c");
	const auto root = put("root.join", leaf.string());
	auto handle = rs->require_sync<joined_resource>(root);
	BOOST_TEST(handle.is_ready());
	BOOST_TEST(handle.get()->str() == "c");
}

// ---------------------------------------------------------------- cycles

BOOST_AUTO_TEST_CASE(CycleBetweenTwoIndependentRoots_FailsBoth)
{
	const auto a = put("a.loop", "memory://t/b.loop");
	const auto b = put("b.loop", "memory://t/a.loop");

	auto a_handle = rs->require<loop_resource>(a);
	auto b_handle = rs->require<loop_resource>(b);
	pump();

	BOOST_TEST(a_handle.has_error());
	BOOST_TEST(b_handle.has_error());
	BOOST_TEST(mentions(a_handle.get_control_block(), "dependency cycle"));
	BOOST_TEST(mentions(b_handle.get_control_block(), "dependency cycle"));
	BOOST_TEST(mentions(a_handle.get_control_block(), "a.loop"));
	BOOST_TEST(mentions(a_handle.get_control_block(), "b.loop"));
}

BOOST_AUTO_TEST_CASE(CycleRequestedFromOneRoot_FailsTheRootAndNamesTheLoop)
{
	const auto a = put("a.loop", "memory://t/b.loop");
	put("b.loop", "memory://t/a.loop"); // only ever requested by a

	auto a_handle = rs->require<loop_resource>(a);
	pump();

	BOOST_TEST(a_handle.has_error());
	BOOST_TEST(mentions(a_handle.get_control_block(), "dependency cycle"));
	BOOST_TEST(mentions(a_handle.get_control_block(), "b.loop"));
}

BOOST_AUTO_TEST_CASE(ResourceThatNeedsItself_FailsWithACycle)
{
	const auto self = put("self.loop", "memory://t/self.loop");
	auto handle = rs->require<loop_resource>(self);
	pump();
	BOOST_TEST(handle.has_error());
	BOOST_TEST(mentions(handle.get_control_block(), "dependency cycle"));
}

BOOST_AUTO_TEST_CASE(CycleOfThree_FailsAllOfItAndWhatWaitsForIt)
{
	const auto a = put("a.loop", "memory://t/b.loop");
	const auto b = put("b.loop", "memory://t/c.loop");
	const auto c = put("c.loop", "memory://t/a.loop");
	const auto outsider = put("outsider.loop", "memory://t/a.loop");

	auto outsider_handle = rs->require<loop_resource>(outsider);
	auto a_handle = rs->require<loop_resource>(a);
	auto b_handle = rs->require<loop_resource>(b);
	auto c_handle = rs->require<loop_resource>(c);
	pump();

	for (const auto& handle : { a_handle, b_handle, c_handle, outsider_handle }) {
		BOOST_TEST(handle.has_error());
		BOOST_TEST(mentions(handle.get_control_block(), "dependency cycle"));
	}
}

BOOST_AUTO_TEST_CASE(AfterACycleWasBroken_TheSameTagsLoadAgain)
{
	const auto a = put("a.loop", "memory://t/b.loop");
	const auto b = put("b.loop", "memory://t/a.loop");
	auto a_handle = rs->require<loop_resource>(a);
	pump();
	BOOST_REQUIRE(a_handle.has_error());

	// The files are fixed and reloaded: nothing of the failed attempt is left in the way.
	rs->store(a, test_util::to_bytes("memory://t/b.loop"));
	rs->store(b, test_util::to_bytes(""));
	rs->signal_changed(a);
	rs->signal_changed(b);
	auto again = rs->require<loop_resource>(a);
	pump();
	BOOST_TEST(again.is_ready());
}

BOOST_AUTO_TEST_CASE(ChainOfThirtyTwoEdges_Loads)
{
	constexpr int edges = 32;
	for (int i = 0; i < edges; ++i) {
		put("n" + std::to_string(i) + ".loop", "memory://t/n" + std::to_string(i + 1) + ".loop");
	}
	put("n" + std::to_string(edges) + ".loop", "");

	auto root = rs->require<loop_resource>(tag_of("n0.loop"));
	pump();
	BOOST_TEST(root.is_ready());
}

BOOST_AUTO_TEST_CASE(ChainOfMoreThanThirtyTwoEdges_FailsTheRootAndStillTerminates)
{
	constexpr int edges = 40;
	for (int i = 0; i < edges; ++i) {
		put("n" + std::to_string(i) + ".loop", "memory://t/n" + std::to_string(i + 1) + ".loop");
	}
	put("n" + std::to_string(edges) + ".loop", "");

	auto root = rs->require<loop_resource>(tag_of("n0.loop"));
	pump();
	BOOST_TEST(root.has_error());
	BOOST_TEST(mentions(root.get_control_block(), "deeper than 32"));
}

// ---------------------------------------------------------------- when_all

BOOST_AUTO_TEST_CASE(WhenAll_WithOneFailingDependency_FailsWhileTheOthersAreDeliveredAndCached)
{
	const auto one = put("one.txt", "1");
	const auto two = put("two.txt", "2");
	auto first = rs->require<res::text_resource>(one);
	auto broken = rs->require<res::text_resource>(tag_of("missing.txt"));
	auto last = rs->require<res::text_resource>(two);

	auto all = res::when_all(*rs, first, broken, last);
	BOOST_TEST(!all.is_ready());
	pump();

	BOOST_TEST(all.has_error());
	BOOST_TEST(mentions(all.get_control_block(), "missing.txt"));
	BOOST_TEST(first.get()->str() == "1");
	BOOST_TEST(last.get()->str() == "2");
	BOOST_TEST(rs->require<res::text_resource>(one).get_control_block() == first.get_control_block());
	BOOST_TEST(rs->require<res::text_resource>(two).get_control_block() == last.get_control_block());
}

BOOST_AUTO_TEST_CASE(WhenAll_OfDifferentKinds_IsReadyWhenAllAre)
{
	const auto text = put("t.txt", "t");
	const auto counted = put("c.cnt", "c");
	auto text_handle = rs->require<res::text_resource>(text);
	auto counted_handle = rs->require<counted_resource>(counted);

	auto all = res::when_all(*rs, text_handle, counted_handle);
	BOOST_TEST(!all.is_ready());
	int called = 0;
	ds::lifetime owner;
	all.then(owner, [&](res::when_all_result&) { ++called; });
	pump();

	BOOST_TEST(all.is_ready());
	BOOST_TEST(called == 1);
}

BOOST_AUTO_TEST_CASE(WhenAll_OfHandlesThatAreAlreadyReady_IsReadyAtOnce)
{
	auto handle = rs->require_sync<res::text_resource>(put("ready.txt", "r"));
	BOOST_TEST(res::when_all(*rs, handle).is_ready());
	BOOST_TEST(res::when_all(*rs, std::vector<res::res_handle<res::text_resource>>{}).is_ready());
}

BOOST_AUTO_TEST_CASE(WhenAll_WithAnInvalidHandle_Fails)
{
	res::res_handle<res::text_resource> none;
	BOOST_TEST(res::when_all(*rs, none).has_error());
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- desc and world adapters

BOOST_FIXTURE_TEST_SUITE(ResDependencyDescAdapterTests, test_util::scene_env)

BOOST_AUTO_TEST_CASE(DescAdapter_DeclaresTheParentAsADependencyOfTypeDescBase)
{
	core::desc::desc_adapter_t adapter(*rs, *descs);
	const auto tag = tag_of("child.desc");

	const auto deps = adapter.collect_dependencies(tag, test_util::to_bytes(R"({"__type":"camera_desc","__parent":"memory://t/parent.desc"})"));
	BOOST_REQUIRE(deps.size() == 1u);
	BOOST_TEST((deps[0].target == tag_of("parent.desc")));
	BOOST_TEST((deps[0].type == ds::type_id::make<desc::desc_base>()));

	BOOST_TEST(adapter.collect_dependencies(tag, test_util::to_bytes(R"({"__type":"camera_desc"})")).empty());
	// "__parent" deeper in the file is not the file's own parent
	BOOST_TEST(adapter.collect_dependencies(tag, test_util::to_bytes(R"({"__type":"camera_desc","note":{"__parent":"x"}})")).empty());
}

BOOST_AUTO_TEST_CASE(DescAdapter_ParentLoadsThroughTheDependency_WithoutRequireSync)
{
	put("parent.desc", R"({"__type":"stat_desc","hp":7,"label":"base"})");
	put("child.desc", R"({"__type":"stat_desc","__parent":"memory://t/parent.desc","label":"own"})");

	auto handle = rs->require<desc::desc_base>(tag_of("child.desc")); // async: nothing waits in the adapter
	pump();

	BOOST_REQUIRE(handle.is_ready());
	const auto loaded = std::dynamic_pointer_cast<test_util::stat_desc>(handle.get());
	BOOST_REQUIRE(loaded);
	BOOST_TEST(loaded->hp == 7);
	BOOST_TEST(loaded->label == "own");
}

BOOST_AUTO_TEST_CASE(DescAdapter_ParentOfItself_IsAnError)
{
	put("loop.desc", R"({"__type":"stat_desc","__parent":"memory://t/loop.desc"})");
	auto handle = rs->require<desc::desc_base>(tag_of("loop.desc"));
	pump();
	BOOST_TEST(handle.has_error());
	BOOST_TEST(mentions(handle.get_control_block(), "cycle"));
}

BOOST_AUTO_TEST_CASE(DescAdapter_ParentCycleOfTwoRootsRequestedAsync_FailsBoth)
{
	put("a.desc", R"({"__type":"stat_desc","__parent":"memory://t/b.desc"})");
	put("b.desc", R"({"__type":"stat_desc","__parent":"memory://t/a.desc"})");
	auto a = rs->require<desc::desc_base>(tag_of("a.desc"));
	auto b = rs->require<desc::desc_base>(tag_of("b.desc"));
	pump();
	BOOST_TEST(a.has_error());
	BOOST_TEST(b.has_error());
	BOOST_TEST(mentions(a.get_control_block(), "dependency cycle"));
	BOOST_TEST(mentions(b.get_control_block(), "dependency cycle"));
}

BOOST_AUTO_TEST_CASE(WorldwrapAdapter_DeclaresTheParentOfAWorldFileOnly)
{
	scn::worldwrap_adapter adapter(*rs, *descs);
	const auto tag = tag_of("child_world.desc");

	const auto deps = adapter.collect_dependencies(tag, test_util::to_bytes(R"({"__type":"world_desc","__parent":"memory://t/parent_world.desc"})"));
	BOOST_REQUIRE(deps.size() == 1u);
	BOOST_TEST((deps[0].target == tag_of("parent_world.desc")));
	BOOST_TEST((deps[0].type == ds::type_id::make<desc::desc_base>()));

	BOOST_TEST(adapter.collect_dependencies(tag, test_util::to_bytes(R"({"__type":"world_desc","name":"w"})")).empty());
	// a prefab wrapped as a world keeps its own "__parent" for the prefab machinery
	BOOST_TEST(adapter.collect_dependencies(tag, test_util::to_bytes(R"({"__type":"prefab_desc","__parent":"memory://t/p.desc"})")).empty());
}

BOOST_AUTO_TEST_CASE(WorldwrapAdapter_ParentLoadsThroughTheDependency_WithoutRequireSync)
{
	rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
	put("parent_world.desc", R"({"__type":"world_desc","name":"base","systems":["a","b"]})");
	put("child_world.desc", R"({"__type":"world_desc","__parent":"memory://t/parent_world.desc","name":"derived"})");

	auto handle = rs->require<scn::world_desc>(tag_of("child_world.desc"));
	pump();

	BOOST_REQUIRE(handle.is_ready());
	BOOST_TEST(handle.get()->get_name() == "derived");
	BOOST_TEST((handle.get()->get_systems() == std::vector<std::string>{ "a", "b" }));
}

BOOST_AUTO_TEST_CASE(WorldwrapAdapter_MissingParent_FailsTheWorld)
{
	rs->registrate_adapter<scn::worldwrap_adapter>(scn::worldwrap_adapter::INFO, std::ref(*rs), std::ref(*descs));
	put("orphan_world.desc", R"({"__type":"world_desc","__parent":"memory://t/nowhere.desc"})");

	auto handle = rs->require<scn::world_desc>(tag_of("orphan_world.desc"));
	pump();

	BOOST_TEST(handle.has_error());
	BOOST_TEST(mentions(handle.get_control_block(), "nowhere.desc"));
}

BOOST_AUTO_TEST_SUITE_END()
