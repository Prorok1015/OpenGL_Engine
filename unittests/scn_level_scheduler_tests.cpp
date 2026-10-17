#include <boost/test/unit_test.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "level/scn_level.h"
#include "scn_ecs_assembler.h"
#include "desc_system.h"
#include "res_system.h"

namespace json = boost::json;

namespace {

	std::vector<std::string> g_log;

	// Resources used only to create read/write dependencies between systems.
	struct res_x {};
	struct res_y {};
	struct res_b {};
	struct res_c {};
	struct res_w {};

	void sys_a(res_x&, res_y&) { g_log.emplace_back("A"); }
	void sys_b(const res_x&, res_b&) { g_log.emplace_back("B"); }
	void sys_c(const res_y&, res_c&) { g_log.emplace_back("C"); }
	void sys_d(const res_b&, const res_c&) { g_log.emplace_back("D"); }

	void sys_w(res_w&) { g_log.emplace_back("W"); }

	struct stateful_sys : ecs::system_interface {
		explicit stateful_sys(std::string tag) : tag(std::move(tag)) {}
		void run(res_w&) { g_log.push_back(tag); }
		std::string tag;
	};

	struct fixture {
		fixture()
			: ds(rs)
			, assembler(ds)
		{
			g_log.clear();
			sf.register_automatic_system<&sys_a>("a");
			sf.register_automatic_system<&sys_b>("b");
			sf.register_automatic_system<&sys_c>("c");
			sf.register_automatic_system<&sys_d>("d");
			sf.register_automatic_system<&sys_w>("w");
			sf.register_automatic_system<&stateful_sys::run, stateful_sys>("stateful", std::string{ "S" });
		}

		scn::world_desc make_world_desc(const std::string& name, const std::vector<std::string>& systems)
		{
			json::object obj;
			obj["name"] = name;
			obj["systems"] = json::value_from(systems);
			scn::world_desc d;
			d.deserialize(ds, obj);
			return d;
		}

		size_t count(const std::string& s) const { return (size_t)std::ranges::count(g_log, s); }
		size_t pos(const std::string& s) const
		{
			return (size_t)std::distance(g_log.begin(), std::ranges::find(g_log, s));
		}

		res::resource_system rs;
		desc::desc_system ds;
		scn::ecs_assembler assembler;
		ecs::system_factory sf;
	};

	constexpr auto one_frame = std::chrono::duration<float>(0.001f); // below the fixed step

}

BOOST_FIXTURE_TEST_SUITE(ScnLevelSchedulerTests, fixture)

BOOST_AUTO_TEST_CASE(Diamond_EachSystemRunsOnceAndInOrder)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);

	lvl.update(one_frame);

	BOOST_TEST(g_log.size() == 4u);
	BOOST_TEST(count("A") == 1u);
	BOOST_TEST(count("B") == 1u);
	BOOST_TEST(count("C") == 1u);
	BOOST_TEST(count("D") == 1u);
	BOOST_TEST(pos("A") < pos("B"));
	BOOST_TEST(pos("A") < pos("C"));
	BOOST_TEST(pos("B") < pos("D"));
	BOOST_TEST(pos("C") < pos("D"));
}

BOOST_AUTO_TEST_CASE(Diamond_OrderIsStableAcrossFrames)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);

	lvl.update(one_frame);
	auto first = g_log;
	g_log.clear();
	lvl.update(one_frame);

	BOOST_TEST(g_log == first);
}

BOOST_AUTO_TEST_CASE(TopologicalOrder_TieBreaksByIndexAndRunsOnce)
{
	scn::level lvl(nullptr);
	auto& w = lvl.create_world("w0", 0);
	lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);

	const auto& graph = lvl.organizer().graph();
	auto order = scn::level::topological_order(graph);

	BOOST_TEST(order == (std::vector<size_t>{ 0, 1, 2, 3 }), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(ReloadWorld_KeepsOtherWorldsSystems)
{
	scn::level lvl(nullptr);
	auto& wa = lvl.create_world("wa", 0);
	lvl.add_world_systems(wa, { "w" }, sf);
	auto& wb = lvl.create_world("wb", 1);
	lvl.add_world_systems(wb, { "stateful" }, sf);

	lvl.update(one_frame);
	BOOST_TEST(count("W") == 1u);
	BOOST_TEST(count("S") == 1u);

	g_log.clear();
	auto desc_b = make_world_desc("wb", { "stateful" });
	lvl.reload_world("wb", desc_b, sf, assembler);
	lvl.update(one_frame);

	BOOST_TEST(count("W") == 1u);
	BOOST_TEST(count("S") == 1u);
	BOOST_TEST(g_log.size() == 2u);
}

BOOST_AUTO_TEST_CASE(LoadClearLoad_DoesNotDuplicateSystems)
{
	scn::level lvl(nullptr);
	{
		auto& w = lvl.create_world("w0", 0);
		lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);
	}
	lvl.update(one_frame);
	BOOST_TEST(g_log.size() == 4u);

	lvl.clear();
	g_log.clear();
	lvl.update(one_frame);
	BOOST_TEST(g_log.empty());

	{
		auto& w = lvl.create_world("w0", 0);
		lvl.add_world_systems(w, { "a", "b", "c", "d" }, sf);
	}
	lvl.update(one_frame);

	BOOST_TEST(g_log.size() == 4u);
	BOOST_TEST(count("A") == 1u);
	BOOST_TEST(count("D") == 1u);
}

BOOST_AUTO_TEST_CASE(MoveAndSwap_LevelStillRunsSystems)
{
	scn::level a(nullptr);
	{
		auto& w = a.create_world("w0", 0);
		a.add_world_systems(w, { "a", "b", "c", "d" }, sf);
	}

	scn::level moved(std::move(a));
	moved.update(one_frame);
	BOOST_TEST(g_log.size() == 4u);

	scn::level other(nullptr);
	{
		auto& w = other.create_world("ow", 0);
		other.add_world_systems(w, { "stateful" }, sf);
	}
	g_log.clear();
	moved.swap(other);

	moved.update(one_frame);
	BOOST_TEST(g_log == (std::vector<std::string>{ "S" }));

	g_log.clear();
	other.update(one_frame);
	BOOST_TEST(g_log.size() == 4u);
}

BOOST_AUTO_TEST_CASE(RemoveWorld_DropsItsSystemsKeepsOthers)
{
	scn::level lvl(nullptr);
	auto& wa = lvl.create_world("wa", 0);
	lvl.add_world_systems(wa, { "w" }, sf);
	auto& wb = lvl.create_world("wb", 1);
	lvl.add_world_systems(wb, { "stateful" }, sf);

	lvl.remove_world("wb", sf);
	lvl.update(one_frame);

	BOOST_TEST(g_log == (std::vector<std::string>{ "W" }));
}

BOOST_AUTO_TEST_CASE(AcquireFreeWorldId_ReusesGapInsteadOfCount)
{
	scn::level lvl(nullptr);
	lvl.create_world("w0", 0);
	lvl.create_world("w1", 1);
	lvl.create_world("w2", 2);
	BOOST_TEST(lvl.acquire_free_world_id() == 3u);

	// Reloading keeps ids; ids 0 and 2 alive with 1 missing must yield 1, not get_world_count() == 2.
	scn::level gap(nullptr);
	gap.create_world("a", 0);
	gap.create_world("c", 2);
	BOOST_TEST(gap.get_world_count() == 2u);
	BOOST_TEST(gap.acquire_free_world_id() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
