#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/scene/scn_model.h>
#include <cstddef>
#include <functional>
#include <iterator>
#include <limits>
#include <vector>

// #927: the range of indices a mesh draws comes from a desc file or from whoever wrote the component. A range outside
// the geometry's buffers is not drawn; the driver would read past them every frame.
namespace {

struct mesh_range_fixture : test_util::null_engine_fixture
{
	rnd::driver::null::driver& gpu()
	{
		// The fixture knows the null driver behind the validating one (render/validate is on in the tests).
		rnd::driver::null::driver* found = driver();
		BOOST_REQUIRE(found != nullptr);
		return *found;
	}

	std::size_t draws_of_a_frame()
	{
		const std::size_t before = gpu().calls().draws;
		BOOST_REQUIRE(run_frames(1) == 1);
		return gpu().calls().draws - before;
	}

	std::size_t each_mesh(const std::function<void(scn::mesh_view&)>& change)
	{
		std::size_t meshes = 0;
		storage.require<scn::context_manager>().level_set().for_each([&](scn::world_context& w) {
			for (auto [e, mesh] : w.state().view<scn::mesh_component>().each()) {
				change(mesh.mesh);
				++meshes;
			}
		});
		return meshes;
	}
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendMeshRange, mesh_range_fixture)

BOOST_AUTO_TEST_CASE(AMeshRangeOutsideItsGeometry_IsNotDrawn)
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_quads.desc")));
	test_util::pump(tasks);
	BOOST_REQUIRE(run_frames(3) == 3);
	const std::size_t whole = draws_of_a_frame();
	BOOST_REQUIRE(whole > 0u);
	BOOST_TEST(draws_of_a_frame() == whole);

	std::vector<scn::mesh_view> kept;
	BOOST_REQUIRE(each_mesh([&kept](scn::mesh_view& mesh) { kept.push_back(mesh); }) > 0u);
	const auto restore = [&] {
		std::size_t index = 0;
		each_mesh([&](scn::mesh_view& mesh) { mesh = kept[index++]; });
	};

	const std::function<void(scn::mesh_view&)> broken[] = {
		// past the end of the index buffer
		[](scn::mesh_view& mesh) { mesh.ind_end = mesh.ind_begin + 100'000'000; },
		// backwards: the count would wrap
		[](scn::mesh_view& mesh) { mesh.ind_begin = mesh.ind_end + 3; },
		// more than a draw call can say
		[](scn::mesh_view& mesh) { mesh.ind_end = std::numeric_limits<std::size_t>::max(); },
		// indices that fit, offset past the last vertex
		[](scn::mesh_view& mesh) { mesh.vx_begin += 100'000'000; },
	};
	std::size_t without_meshes = whole;
	for (std::size_t i = 0; i < std::size(broken); ++i) {
		BOOST_TEST_CONTEXT("broken range " << i) {
			each_mesh(broken[i]);
			const std::size_t drawn = draws_of_a_frame();
			BOOST_TEST(drawn < whole);
			if (i > 0) BOOST_TEST(drawn == without_meshes);
			without_meshes = drawn;
			restore();
			BOOST_TEST(draws_of_a_frame() == whole);
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
