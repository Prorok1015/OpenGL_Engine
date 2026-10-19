#include <boost/json.hpp>
#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/common/eng_profiler.h>
#include <snk/common/logger/engine_log.h>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_model.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

// Extract benchmark of docs/roadmap/design/RHI.md section 4 ("Замер"): the engine frame on the null backend
// over levels/bench_100_meshes.desc (100 meshes, 10 of them skinned, 2 cameras), so the numbers carry no GPU
// noise. Disabled by default: run it by name, in an optimized build with the internal profiler
//
//   unit_tests --run_test=AppEngineNullBackendBench
//
// The report goes to the log (category "bench") and to the JSON file named by SNK_BENCH_JSON
// (default: bench_extract.json in the working directory).

namespace {

namespace tt = boost::unit_test;

constexpr int warmup_frames = 30;
constexpr int measured_frames = 300;
constexpr std::uint32_t expected_mesh_count = 100;
constexpr std::uint32_t expected_camera_count = 2;
constexpr std::size_t expected_skinned_count = 10;

struct zone_series
{
	std::vector<double> total_ms;
	std::uint32_t calls_per_frame = 0;
};

struct summary
{
	double mean = 0.0;
	double p50 = 0.0;
	double p95 = 0.0;
	double max = 0.0;
};

summary summarize(std::vector<double> values)
{
	summary out;
	if (values.empty()) return out;
	std::ranges::sort(values);
	const auto nearest_rank = [&values](double q) {
		const auto rank = static_cast<std::size_t>(std::ceil(q * static_cast<double>(values.size())));
		return values[std::clamp<std::size_t>(rank, 1, values.size()) - 1];
	};
	out.mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
	out.p50 = nearest_rank(0.50);
	out.p95 = nearest_rank(0.95);
	out.max = values.back();
	return out;
}

bool is_extract_zone(std::string_view name)
{
	return name == "ExtractRenderData" || name.starts_with("Extract.");
}

// Per-frame total (all calls) of every extract zone recorded on this thread in the current frame. Taken right
// after the extract stage: the thread's ring of entries is short, and a whole frame of zones would wrap it.
std::map<std::string, std::pair<double, std::uint32_t>> extract_zones_of_current_frame()
{
	std::map<std::string, std::pair<double, std::uint32_t>> zones;
	const ds::profiler_thread_data& td = ds::profiler_get_thread_data();
	for (std::uint32_t i = 0; i < td.entry_count(); ++i) {
		const ds::profile_entry& e = td.entries[i];
		if (e.frame_number != td.frame_number || !e.name || !is_extract_zone(e.name)) continue;
		auto& [total_us, calls] = zones[e.name];
		total_us += e.duration_us;
		++calls;
	}
	return zones;
}

struct bench_fixture : test_util::null_engine_fixture
{
	bench_fixture() : null_engine_fixture(false) {}
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendBench, bench_fixture)

BOOST_AUTO_TEST_CASE(ExtractRenderDataOnHundredMeshes, * tt::disabled())
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/bench_100_meshes.desc")));
	BOOST_REQUIRE(levels.get_level().get_world_count() > 0u);

	std::size_t skinned = 0;
	levels.get_level().for_each_world([&skinned](scn::world& w) {
		skinned += w.state().view<scn::skinning_component>().size();
	});
	BOOST_TEST(skinned == expected_skinned_count);

	std::map<std::string, zone_series> series;
	int frame = 0;
	auto& stages = storage.require<app::frame_stage_registry>();
	stages.add_stage("bench_sample_extract", app::builtin_stage::extract, app::stage_side::after,
		[&](app::stage_context&) {
			if (frame++ < warmup_frames) return;
			for (const auto& [name, zone] : extract_zones_of_current_frame()) {
				auto& s = series[name];
				s.total_ms.push_back(zone.first / 1000.0);
				s.calls_per_frame = zone.second;
			}
		});
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(warmup_frames + measured_frames) == warmup_frames + measured_frames));
	stages.remove_stage("bench_sample_extract");

	BOOST_REQUIRE(series.contains("ExtractRenderData"));
	BOOST_TEST(series.at("ExtractRenderData").total_ms.size() == static_cast<std::size_t>(measured_frames));
	BOOST_TEST(series.at("ExtractRenderData").calls_per_frame == 1u);
	for (const char* zone : { "Extract.BuildDC", "Extract.Skinning", "Extract.ShaderDesc", "Extract.PushDraws" }) {
		BOOST_REQUIRE_MESSAGE(series.contains(zone), zone << " was not recorded");
		BOOST_TEST_CONTEXT(zone) {
			BOOST_TEST(series.at(zone).calls_per_frame == expected_mesh_count * expected_camera_count);
		}
	}

	json::object zones;
	egLOG_INFO("bench", "extract, {} frames after {} warm-up, {} meshes, {} cameras (per-frame totals, ms)",
		measured_frames, warmup_frames, expected_mesh_count, expected_camera_count);
	egLOG_INFO("bench", "{:<24} {:>9} {:>9} {:>9} {:>9} {:>7}", "zone", "mean", "p50", "p95", "max", "calls");
	for (const auto& [name, s] : series) {
		const summary sum = summarize(s.total_ms);
		egLOG_INFO("bench", "{:<24} {:>9.4f} {:>9.4f} {:>9.4f} {:>9.4f} {:>7}", name, sum.mean, sum.p50, sum.p95, sum.max, s.calls_per_frame);
		zones[name] = json::object{
			{ "mean_ms", sum.mean }, { "p50_ms", sum.p50 }, { "p95_ms", sum.p95 }, { "max_ms", sum.max },
			{ "calls_per_frame", s.calls_per_frame } };
	}

#ifdef NDEBUG
	constexpr std::string_view build_type = "optimized";
#else
	constexpr std::string_view build_type = "unoptimized";
#endif
	const json::object report{
		{ "scene", "levels/bench_100_meshes.desc" },
		{ "backend", "null" },
		{ "build", build_type },
		{ "meshes", expected_mesh_count },
		{ "cameras", expected_camera_count },
		{ "warmup_frames", warmup_frames },
		{ "frames", measured_frames },
		{ "zones", zones } };

	const char* path_env = std::getenv("SNK_BENCH_JSON");
	const std::string path = path_env && *path_env ? path_env : "bench_extract.json";
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << json::serialize(report) << '\n';
	BOOST_TEST(out.good());
	egLOG_INFO("bench", "report written to {}", path);
}

BOOST_AUTO_TEST_SUITE_END()
