#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_model.h>
#include <functional>
#include <vector>

// #560: the render data extractor draws a mesh only while it has scn::renderable (scn::set_visible).

namespace {

struct mesh_ref
{
	entt::registry* reg = nullptr;
	entt::entity ent = entt::null;
};

struct mesh_visible_fixture : test_util::null_engine_fixture
{
	mesh_ref first_drawn_mesh()
	{
		mesh_ref out;
		storage.require<scn::context_manager>().level_set().for_each([&out](scn::world_context& w) {
			if (out.reg) return;
			const auto drawn = w.state().view<scn::material_desc_component, scn::mesh_component, scn::renderable>();
			for (const auto ent : drawn) {
				if (drawn.get<scn::material_desc_component>(ent).mlt_desc.get()) {
					out = { &w.state(), ent };
					return;
				}
			}
		});
		return out;
	}

	std::size_t drawn_in_this_frame()
	{
		std::size_t draws = 0;
		auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
		for (const auto& packet : packets) {
			draws += packet.opaque_draws.size() + packet.transparent_draws.size();
		}
		return draws;
	}

	// before[i] runs ahead of the extract stage of frame i; the draws of every frame are counted right behind it.
	std::vector<std::size_t> draws_per_frame(std::vector<std::function<void()>> before)
	{
		std::vector<std::size_t> draws;
		auto& stages = storage.require<app::frame_stage_registry>();
		int frame = 0;
		stages.add_stage("mesh_visible_before", app::builtin_stage::extract, app::stage_side::before,
			[&](app::stage_context&) {
				if (frame < static_cast<int>(before.size()) && before[frame]) before[frame]();
				++frame;
			});
		stages.add_stage("mesh_visible_after", app::builtin_stage::extract, app::stage_side::after,
			[&](app::stage_context&) { draws.push_back(drawn_in_this_frame()); });
		BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(static_cast<int>(before.size())) == static_cast<int>(before.size())));
		stages.remove_stage("mesh_visible_before");
		stages.remove_stage("mesh_visible_after");
		return draws;
	}

	void load_bench_level()
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/bench_100_meshes.desc")));
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(3) == 3);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendMeshVisible, mesh_visible_fixture)

BOOST_AUTO_TEST_CASE(AMeshWithoutTheRenderableMark_IsNotInTheRenderData_AndComesBackWithIt)
{
	load_bench_level();
	const mesh_ref mesh = first_drawn_mesh();
	BOOST_REQUIRE(mesh.reg);

	const std::vector<std::size_t> draws = draws_per_frame({
		nullptr,
		[&] { scn::set_visible(*mesh.reg, mesh.ent, false); },
		[&] { scn::set_visible(*mesh.reg, mesh.ent, true); },
	});
	BOOST_REQUIRE(draws.size() == 3u);
	BOOST_TEST(draws[0] > 0u);
	BOOST_TEST(draws[1] < draws[0]);
	BOOST_TEST(draws[2] == draws[0]);
}

BOOST_AUTO_TEST_SUITE_END()
