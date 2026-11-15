#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "geom/rnd_geom_manager.h"
#include <snk/desc/desc_base.hpp>
#include <snk/desc/desc_system.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_scene_layer.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

// #1021: a geometry is loaded by the root resource system, so every layer, world and the renderer share one block and
// the file is read once.
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

struct other_desc : desc::desc_base
{
	void deserialize(desc::desc_system&, const boost::json::object&) override {}
	void serialize(boost::json::object&) const override {}
};

struct geometry_root_fixture : test_util::null_engine_fixture
{
	geometry_root_fixture()
	{
		root().unregistrate_resolver(res::tag::memory);
		memory = &root().registrate_resolver<counting_resolver>(res::tag::memory);
	}

	res::resource_system& root() { return storage.require<res::resource_system>(); }
	scn::context_manager& contexts() { return storage.require<scn::context_manager>(); }
	res::resource_system& game_resources() { return contexts().game_layer()->resources(); }
	rnd::driver::driver_interface* gpu() { return storage.require<rnd::render_system>().get_driver(); }

	res::tag put(std::string_view name, std::string_view text)
	{
		const res::tag tag{ std::string{ "memory://geometry_root/" }.append(name) };
		BOOST_REQUIRE(root().store(tag, test_util::to_bytes(text)));
		test_util::pump(tasks);
		memory->reads.erase(tag);
		return tag;
	}

	static std::string triangle()
	{
		std::string text = R"json({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[0,1,2],)json";
		text += R"json("vertices":[)json";
		for (std::size_t i = 0; i < 3 * 12; ++i) {
			text += i == 0 ? "0" : ",0";
		}
		text += "]}";
		return text;
	}

	res::tag put_triangle(std::string_view name) { return put(name, triangle()); }

	template<class T>
	res::res_handle<T> load(res::resource_system& resources, const res::tag& tag)
	{
		auto handle = resources.require_sync<T>(tag);
		test_util::pump(tasks);
		return handle;
	}

	std::size_t reads(const res::tag& tag) const
	{
		const auto found = memory->reads.find(tag);
		return found == memory->reads.end() ? 0u : found->second;
	}

	rnd::driver::vertex_array_interface* build(rnd::geom_manager& geometry, const res::tag& tag)
	{
		rnd::driver::vertex_array_interface* built = nullptr;
		for (int attempt = 0; attempt < 16 && !(built = geometry.require_geometry(tag)); ++attempt) {
			test_util::pump(tasks);
		}
		return built;
	}

	counting_resolver* memory = nullptr;
};

}

BOOST_FIXTURE_TEST_SUITE(RndGeometryRoot, geometry_root_fixture)

BOOST_AUTO_TEST_CASE(AGeometryAskedForThroughTheGameLayer_IsTheRootsBlock)
{
	const res::tag tag = put_triangle("layer.desc");
	const std::size_t layer_live = game_resources().live_resources();

	const auto through_layer = load<rnd::geometry_desc>(game_resources(), tag);
	const auto through_root = load<rnd::geometry_desc>(root(), tag);
	BOOST_REQUIRE(through_layer.is_ready());
	BOOST_TEST((through_layer.get_control_block() == through_root.get_control_block()));
	BOOST_TEST(reads(tag) == 1u);
	BOOST_TEST(game_resources().live_resources() == layer_live);
}

BOOST_AUTO_TEST_CASE(TwoLayersAndTheRenderer_ReadTheGeometryOnce_AndBuildOneVertexArray)
{
	const res::tag tag = put_triangle("shared.desc");
	auto& second = contexts().create_layer();
	const std::size_t second_live = second.resources().live_resources();
	{
		const auto in_game = load<rnd::geometry_desc>(game_resources(), tag);
		const auto in_second = load<rnd::geometry_desc>(second.resources(), tag);
		BOOST_TEST((in_game.get_control_block() == in_second.get_control_block()));

		rnd::geom_manager geometry(gpu(), root());
		auto* const built = build(geometry, tag);
		BOOST_REQUIRE(built != nullptr);
		BOOST_TEST(geometry.require_geometry(tag) == built);
		BOOST_TEST(reads(tag) == 1u);
		// Nothing of the geometry is the second layer's: destroying it names nothing as still held.
		BOOST_TEST(second.resources().live_resources() == second_live);
	}
	contexts().destroy_layer(second);
	BOOST_TEST(reads(tag) == 1u);
}

// A module's layer writes under its own prefix; the geometry it wrote is still loaded by the root, so destroying the
// layer finds none of it held in the layer.
BOOST_AUTO_TEST_CASE(AGeometryAModuleLayerWrote_IsTheRootsBlock)
{
	auto& module = contexts().create_layer();
	module.resources().limit_writes_to("memory://modules/geometry_root/");
	const std::size_t module_live = module.resources().live_resources();
	const res::tag tag{ "memory://modules/geometry_root/written.geom.desc" };
	BOOST_REQUIRE(module.resources().store(tag, test_util::to_bytes(triangle())));
	test_util::pump(tasks);
	memory->reads.erase(tag);
	{
		const auto in_module = load<rnd::geometry_desc>(module.resources(), tag);
		const auto in_root = load<rnd::geometry_desc>(root(), tag);
		BOOST_REQUIRE(in_module.is_ready());
		BOOST_TEST((in_module.get_control_block() == in_root.get_control_block()));
		BOOST_TEST(reads(tag) == 1u);
		BOOST_TEST(module.resources().live_resources() == module_live);
	}
	contexts().destroy_layer(module);
}

BOOST_AUTO_TEST_CASE(AChangedGeometry_IsReadOnceMore_ThroughTheLayer)
{
	const res::tag tag = put_triangle("changed.desc");
	const auto before = load<rnd::geometry_desc>(game_resources(), tag);
	BOOST_REQUIRE(before.is_ready());

	int told = 0;
	game_resources().watch(tag, this, [&told](const res::tag&) { ++told; });
	root().signal_changed(tag);
	test_util::pump(tasks);
	BOOST_TEST(told == 1);
	game_resources().unwatch(tag, this);

	const auto after_layer = load<rnd::geometry_desc>(game_resources(), tag);
	const auto after_root = load<rnd::geometry_desc>(root(), tag);
	BOOST_REQUIRE(after_layer.is_ready());
	BOOST_TEST((after_layer.get_control_block() != before.get_control_block()));
	BOOST_TEST((after_layer.get_control_block() == after_root.get_control_block()));
	BOOST_TEST(reads(tag) == 2u);
}

// The geometry's "__parent" is a dependency in the root: its change reaches the geometry asked for through a layer.
BOOST_AUTO_TEST_CASE(AChangedParent_IsFollowedByTheGeometry)
{
	const res::tag parent = put_triangle("parent.desc");
	const res::tag tag = put("child.desc",
		R"json({"__type":"geometry_desc","__parent":"memory://geometry_root/parent.desc",)json"
		R"json("layout":{},"indices":[],"vertices":[]})json");
	const auto before = load<rnd::geometry_desc>(game_resources(), tag);
	BOOST_REQUIRE(before.is_ready());
	BOOST_REQUIRE(reads(tag) == 1u);

	root().signal_changed(parent);
	test_util::pump(tasks);
	const auto after = load<rnd::geometry_desc>(game_resources(), tag);
	BOOST_REQUIRE(after.is_ready());
	BOOST_TEST((after.get_control_block() != before.get_control_block()));
	BOOST_TEST(reads(tag) == 2u);
}

BOOST_AUTO_TEST_CASE(ADescOfAnotherType_FailsAsAGeometry)
{
	auto& descs = storage.require<desc::desc_system>();
	descs.register_desc<other_desc>("geometry_root_other_desc");
	{
		const res::tag tag = put("other.desc", R"json({"__type":"geometry_root_other_desc"})json");
		const auto as_geometry = load<rnd::geometry_desc>(game_resources(), tag);
		BOOST_TEST(as_geometry.has_error());
		BOOST_TEST(!as_geometry.is_ready());
	}
	descs.unregister_desc("geometry_root_other_desc");
}

BOOST_AUTO_TEST_SUITE_END()
