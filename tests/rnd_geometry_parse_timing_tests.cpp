#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/level/scn_context_manager.h>
#include <boost/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// #1022: a temporary measurement of where a model's geometry desc spends its load, run by CI on Windows Debug; it
// checks nothing and goes once the numbers are in the PR.
namespace timing_1022 {

// The conversion the vertices went through before #1022: value_to of the whole array, one tag_invoke per element.
struct old_byte
{
	std::byte value{};
};

old_byte tag_invoke(boost::json::value_to_tag<old_byte>, const boost::json::value& jv)
{
	if (jv.is_uint64() && jv.as_uint64() <= 255) return { std::byte(jv.as_uint64()) };
	if (jv.is_int64() && jv.as_int64() >= 0 && jv.as_int64() <= 255) return { std::byte(jv.as_int64()) };
	throw std::runtime_error("not a byte");
}

template<class F>
double best_ms(int runs, F&& run)
{
	double best = 1e300;
	for (int i = 0; i < runs; ++i) {
		const auto start = std::chrono::steady_clock::now();
		run();
		const std::chrono::duration<double, std::milli> took = std::chrono::steady_clock::now() - start;
		best = std::min(best, took.count());
	}
	return best;
}

}

BOOST_FIXTURE_TEST_SUITE(RndGeometryParseTiming, test_util::null_engine_fixture)

BOOST_AUTO_TEST_CASE(TheRobotsGeometry_Timings, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	using namespace timing_1022;
	auto& resources = storage.require<res::resource_system>();
	BOOST_REQUIRE(storage.require<scn::context_manager>().load_level(res::tag::make("levels/smoke_level.desc")));
	test_util::pump(tasks);

	const res::tag geometry{ "memory://objects/robot/gen_robot.geom.desc" };
	auto raw = resources.fetch_data(geometry);
	BOOST_REQUIRE(raw);
	raw->wait();
	BOOST_REQUIRE(raw->is_ready());
	const std::vector<std::byte> bytes = raw->data;
	const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());

	boost::json::value parsed;
	const double parse_ms = best_ms(3, [&] { parsed = boost::json::parse(text); });
	const boost::json::value& vertices_value = parsed.as_object().at("vertices");
	const boost::json::array& vertices = vertices_value.as_array();

	std::size_t old_size = 0;
	const double old_vertices_ms = best_ms(3, [&] {
		old_size = boost::json::value_to<std::vector<old_byte>>(vertices_value).size();
	});

	// The loop geometry_desc::deserialize now runs, as written there.
	std::size_t new_size = 0;
	const double new_vertices_ms = best_ms(3, [&] {
		std::vector<std::byte> read;
		read.reserve(vertices.size());
		for (const boost::json::value& item : vertices) {
			read.push_back(tag_invoke(boost::json::value_to_tag<old_byte>{}, item).value);
		}
		new_size = read.size();
	});

	int copy = 0;
	const double load_ms = best_ms(3, [&] {
		const res::tag fresh{ std::string{ "memory://timing_1022/robot_" } + std::to_string(++copy) + ".geom.desc" };
		BOOST_REQUIRE(resources.store(fresh, bytes));
		test_util::pump(tasks);
		const auto loaded = resources.require_sync<rnd::geometry_desc>(fresh);
		BOOST_REQUIRE(loaded.is_ready());
	});

	egLOG_INFO("test/timing", "#1022 robot geometry: {} bytes of JSON, {} vertex bytes", bytes.size(), vertices.size());
	egLOG_INFO("test/timing", "#1022 boost::json::parse of the text: {:.1f} ms", parse_ms);
	egLOG_INFO("test/timing", "#1022 old vertices (value_to per element): {:.1f} ms, {} bytes", old_vertices_ms, old_size);
	egLOG_INFO("test/timing", "#1022 new vertices (one loop, reserved): {:.1f} ms, {} bytes", new_vertices_ms, new_size);
	egLOG_INFO("test/timing", "#1022 new full load (store + require_sync, parse included): {:.1f} ms", load_ms);
	BOOST_TEST_MESSAGE("#1022 timings: parse " << parse_ms << " ms, old vertices " << old_vertices_ms << " ms, new load "
		<< load_ms << " ms, new vertices " << new_vertices_ms << " ms");
}

BOOST_AUTO_TEST_SUITE_END()
