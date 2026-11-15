#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "geom/rnd_geom_manager.h"
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

// #1009 K1: geom_manager follows a change of a geometry desc without a frame missing the mesh, and lets go of the
// vertex arrays nobody draws.
namespace {

class counting_resolver : public test_util::ram_resolver
{
public:
	using test_util::ram_resolver::ram_resolver;

	async_raw_data resolve(const res::tag& tag) const override
	{
		++reads[tag];
		return test_util::ram_resolver::resolve(tag);
	}

	mutable std::unordered_map<res::tag, std::size_t> reads;
};

struct geom_reload_fixture : test_util::null_engine_fixture
{
	geom_reload_fixture()
	{
		root().unregistrate_resolver(res::tag::memory);
		memory = &root().registrate_resolver<counting_resolver>(res::tag::memory);
	}

	res::resource_system& root() { return storage.require<res::resource_system>(); }
	rnd::driver::driver_interface* gpu() { return storage.require<rnd::render_system>().get_driver(); }
	std::size_t live_vertex_arrays() { return driver()->stats().vertex_arrays.live(); }
	std::size_t live_buffers() { return driver()->stats().buffers.live(); }

	// A triangle list of `triangles` triangles over 3 * `triangles` vertices.
	static std::string triangles_text(int triangles)
	{
		std::string indices;
		std::string vertices;
		for (int i = 0; i < 3 * triangles; ++i) {
			indices += (i == 0 ? "" : ",") + std::to_string(i);
			for (int byte = 0; byte < 12; ++byte) {
				vertices += (i == 0 && byte == 0) ? "0" : ",0";
			}
		}
		return R"json({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[)json" + indices
			+ R"json(],"vertices":[)json" + vertices + "]}";
	}

	void write(const res::tag& tag, std::string_view text)
	{
		BOOST_REQUIRE(root().store(tag, test_util::to_bytes(text)));
		test_util::pump(tasks);
	}

	void change(const res::tag& tag, std::string_view text)
	{
		write(tag, text);
		root().signal_changed(tag);
	}

	// One frame of the manager: the main-thread queue (the watchers), then the frame's start and a draw.
	rnd::driver::vertex_array_interface* frame(rnd::geom_manager& geometry, const res::tag& tag)
	{
		test_util::pump(tasks);
		geometry.begin_frame();
		return geometry.require_geometry(tag);
	}

	rnd::driver::vertex_array_interface* build(rnd::geom_manager& geometry, const res::tag& tag)
	{
		rnd::driver::vertex_array_interface* built = nullptr;
		for (int attempt = 0; attempt < 16 && !(built = frame(geometry, tag)); ++attempt) {}
		return built;
	}

	std::size_t reads(const res::tag& tag) const
	{
		const auto found = memory->reads.find(tag);
		return found == memory->reads.end() ? 0u : found->second;
	}

	counting_resolver* memory = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(RndGeomManagerReload, geom_reload_fixture)

BOOST_AUTO_TEST_CASE(AChangedGeometry_IsDrawnOnEveryFrame_ThePreviousOneUntilTheNewIsBuilt)
{
	const res::tag tag{ "memory://geom_reload/changed.geom.desc" };
	write(tag, triangles_text(1));
	rnd::geom_manager geometry(gpu(), root());
	rnd::driver::vertex_array_interface* const before = build(geometry, tag);
	BOOST_REQUIRE(before != nullptr);
	BOOST_TEST(!geometry.draw_fits(tag, 6, 0, 0));

	change(tag, triangles_text(2));
	bool rebuilt = false;
	for (int i = 0; i < 16 && !rebuilt; ++i) {
		BOOST_TEST_CONTEXT("frame " << i) {
			rnd::driver::vertex_array_interface* const drawn = frame(geometry, tag);
			BOOST_REQUIRE(drawn != nullptr);
			rebuilt = drawn != before;
			// Until the swap the previous geometry answers for itself.
			if (!rebuilt) BOOST_TEST(!geometry.draw_fits(tag, 6, 0, 0));
		}
	}
	BOOST_REQUIRE(rebuilt);
	BOOST_TEST(geometry.draw_fits(tag, 6, 0, 0));
}

BOOST_AUTO_TEST_CASE(AChangeThatFailsToLoad_KeepsThePreviousGeometry_AndTheNextChangeIsTried)
{
	const res::tag tag{ "memory://geom_reload/broken.geom.desc" };
	write(tag, triangles_text(1));
	rnd::geom_manager geometry(gpu(), root());
	rnd::driver::vertex_array_interface* const before = build(geometry, tag);
	BOOST_REQUIRE(before != nullptr);

	change(tag, "not a desc");
	for (int i = 0; i < 16; ++i) {
		BOOST_TEST_CONTEXT("frame " << i) { BOOST_TEST(frame(geometry, tag) == before); }
	}
	BOOST_TEST(!geometry.draw_fits(tag, 6, 0, 0));

	// The watch stays: a later good change is drawn.
	change(tag, triangles_text(2));
	for (int i = 0; i < 16 && geometry.require_geometry(tag) == before; ++i) {
		BOOST_REQUIRE(frame(geometry, tag) != nullptr);
	}
	BOOST_TEST(geometry.draw_fits(tag, 6, 0, 0));
}

BOOST_AUTO_TEST_CASE(ReloadsOfOneGeometry_LeaveNoVertexArrayOrBufferBehind)
{
	const res::tag tag{ "memory://geom_reload/many.geom.desc" };
	write(tag, triangles_text(1));
	rnd::geom_manager geometry(gpu(), root());
	BOOST_REQUIRE(build(geometry, tag) != nullptr);
	const std::size_t arrays = live_vertex_arrays();
	const std::size_t buffers = live_buffers();

	for (int reload = 0; reload < 5; ++reload) {
		change(tag, triangles_text(1 + reload % 2));
		rnd::driver::vertex_array_interface* const before = geometry.find_geometry(tag);
		for (int i = 0; i < 16 && frame(geometry, tag) == before; ++i) {}
		BOOST_TEST_CONTEXT("reload " << reload) {
			BOOST_TEST(live_vertex_arrays() == arrays);
			BOOST_TEST(live_buffers() == buffers);
		}
	}
}

BOOST_AUTO_TEST_CASE(AGeometryNotDrawn_IsLetGo_AndBuiltAgainWithoutReadingTheFile)
{
	const res::tag tag{ "memory://geom_reload/unused.geom.desc" };
	write(tag, triangles_text(1));
	const std::size_t watched = root().watched_tag_count();
	{
		rnd::geom_manager geometry(gpu(), root());
		// What an entity's geometry_holds keeps: the block stays while the vertex array goes.
		const auto held = root().require_sync<rnd::geometry_desc>(tag);
		BOOST_REQUIRE(held.is_ready());
		BOOST_REQUIRE(build(geometry, tag) != nullptr);
		BOOST_TEST(root().watched_tag_count() == watched + 1u);
		const std::size_t read = reads(tag);
		const std::size_t arrays = live_vertex_arrays();

		for (std::uint64_t i = 0; i <= rnd::k_unused_frames_kept; ++i) {
			geometry.begin_frame();
		}
		BOOST_TEST(geometry.find_geometry(tag) == nullptr);
		BOOST_TEST(live_vertex_arrays() == arrays - 1u);
		BOOST_TEST(root().watched_tag_count() == watched);

		BOOST_TEST(build(geometry, tag) != nullptr);
		BOOST_TEST(reads(tag) == read);
		BOOST_TEST(root().watched_tag_count() == watched + 1u);
	}
	// The destructor lets go of its watches.
	BOOST_TEST(root().watched_tag_count() == watched);
}

BOOST_AUTO_TEST_CASE(AGeometryDrawnEveryFrame_StaysPastTheUnusedFrames)
{
	const res::tag tag{ "memory://geom_reload/drawn.geom.desc" };
	write(tag, triangles_text(1));
	rnd::geom_manager geometry(gpu(), root());
	rnd::driver::vertex_array_interface* const built = build(geometry, tag);
	BOOST_REQUIRE(built != nullptr);
	for (std::uint64_t i = 0; i <= rnd::k_unused_frames_kept + 1; ++i) {
		geometry.begin_frame();
		BOOST_REQUIRE(geometry.require_geometry(tag) == built);
	}
}

// What a game module writes under its prefix reaches the renderer through the root's watch like a file does.
BOOST_AUTO_TEST_CASE(AGeometryAModuleRewrites_IsDrawnAnew)
{
	auto& module = storage.require<scn::context_manager>().create_layer();
	module.resources().limit_writes_to("memory://modules/geom_reload/");
	const res::tag tag{ "memory://modules/geom_reload/mesh.geom.desc" };
	BOOST_REQUIRE(module.resources().store(tag, test_util::to_bytes(triangles_text(1))));
	{
		rnd::geom_manager geometry(gpu(), root());
		rnd::driver::vertex_array_interface* const before = build(geometry, tag);
		BOOST_REQUIRE(before != nullptr);

		BOOST_REQUIRE(module.resources().store(tag, test_util::to_bytes(triangles_text(2))));
		module.resources().signal_changed(tag);
		for (int i = 0; i < 16 && frame(geometry, tag) == before; ++i) {}
		BOOST_TEST(geometry.draw_fits(tag, 6, 0, 0));
	}
	storage.require<scn::context_manager>().destroy_layer(module);
}

BOOST_AUTO_TEST_SUITE_END()
