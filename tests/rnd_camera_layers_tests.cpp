#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "rnd_null_driver.h"
#include "rnd_opaque_pass.h"
#include "rnd_test_render_context.hpp"
#include "rnd_z_prepass.h"
#include <snk/common/mem_allocator.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_render_pass_interface.h>
#include <snk/render/rnd_render_system.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <algorithm>
#include <cmath>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Render layers (WORLD_MESSAGING section 7, W4 #272): the camera's layer, clear and projection reach the packets,
// render_system::render_frame draws the packets one priority group after the other, and the opaque pass and the
// z-prepass honour the clear mode. Everything runs on the null backend.

namespace vl = rnd::driver::validation;

namespace {

using namespace rnd;

render_packet_t make_packet(int priority, clear_mode clear, glm::ivec4 viewport = { 0, 0, 64, 64 })
{
	render_packet_t packet;
	packet.camera.viewport = viewport;
	packet.camera.render_priority = priority;
	packet.camera.clear = clear;
	return packet;
}

std::vector<int> priorities_of(std::span<render_packet_t> packets)
{
	std::vector<int> out;
	for (const auto& packet : packets) out.push_back(packet.camera.render_priority);
	return out;
}

// The x of the viewport stands for the packet in the order checks.
std::vector<int> ids_of(std::span<render_packet_t> packets)
{
	std::vector<int> out;
	for (const auto& packet : packets) out.push_back(packet.camera.viewport.x);
	return out;
}

}

BOOST_AUTO_TEST_SUITE(RndCameraLayersTests)

// ---------------------------------------------------------------- the order of the groups

BOOST_AUTO_TEST_CASE(PacketsAreSortedByPriorityAndKeepTheExtractionOrderWithinOne)
{
	ds::frame_allocator_reset();
	std::pmr::vector<render_packet_t> packets(ds::frame_allocator());
	packets.reserve(5);
	const int priorities[] = { 1, 0, 1, 0, -2 };
	for (int i = 0; i < 5; ++i) {
		packets.push_back(make_packet(priorities[i], clear_mode::all, { i, 0, 64, 64 }));
	}
	sort_packets_by_priority(packets);
	BOOST_TEST((priorities_of(packets) == std::vector<int>{ -2, 0, 0, 1, 1 }));
	BOOST_TEST((ids_of(packets) == std::vector<int>{ 4, 1, 3, 0, 2 }));
}

BOOST_AUTO_TEST_CASE(GroupsAreVisitedLowestPriorityFirstAndHoldOnlyTheirOwnPackets)
{
	ds::frame_allocator_reset();
	std::pmr::vector<render_packet_t> packets(ds::frame_allocator());
	packets.reserve(5);
	for (const int priority : { 2, 0, 2, 1, 0 }) {
		packets.push_back(make_packet(priority, clear_mode::all));
	}
	sort_packets_by_priority(packets);

	std::vector<std::vector<int>> groups;
	for_each_priority_group(packets, [&](std::span<render_packet_t> group) { groups.push_back(priorities_of(group)); });
	BOOST_REQUIRE_EQUAL(groups.size(), 3u);
	BOOST_TEST((groups[0] == std::vector<int>{ 0, 0 }));
	BOOST_TEST((groups[1] == std::vector<int>{ 1 }));
	BOOST_TEST((groups[2] == std::vector<int>{ 2, 2 }));
}

BOOST_AUTO_TEST_CASE(NoPacketsMeansNoGroups)
{
	std::size_t visited = 0;
	for_each_priority_group({}, [&](std::span<render_packet_t>) { ++visited; });
	BOOST_TEST(visited == 0u);
}

// ---------------------------------------------------------------- clear flags

BOOST_AUTO_TEST_CASE(ClearModeNamesWhatIsCleared)
{
	BOOST_TEST(clears_color(clear_mode::all));
	BOOST_TEST(clears_depth(clear_mode::all));
	BOOST_TEST(!clears_color(clear_mode::depth));
	BOOST_TEST(clears_depth(clear_mode::depth));
	BOOST_TEST(!clears_color(clear_mode::none));
	BOOST_TEST(!clears_depth(clear_mode::none));
}

namespace {

struct clear_case
{
	clear_mode mode;
	driver::load_op color;
	driver::load_op depth;
};

constexpr clear_case clear_cases[] = {
	{ clear_mode::all,   driver::load_op::clear, driver::load_op::clear },
	{ clear_mode::depth, driver::load_op::load,  driver::load_op::clear },
	{ clear_mode::none,  driver::load_op::load,  driver::load_op::load },
};

struct single_camera
{
	// Reset the frame allocator before making one: the packets live in it.
	single_camera(clear_mode mode, bool offscreen)
		: packets(ds::frame_allocator())
	{
		render_packet_t packet = make_packet(0, mode, { 8, 16, 64, 32 });
		if (offscreen) {
			packet.camera.color_targets.push_back(res::tag(res::tag::memory, "__camera_layers_rt"));
		}
		packets.push_back(std::move(packet));
		context.use_packets(&packets);
	}

	driver::null::driver drv;
	test_util::test_render_context context;
	std::pmr::vector<render_packet_t> packets;
};

}

BOOST_AUTO_TEST_CASE(OpaquePassClearsTheBackbufferAsTheCameraAsks)
{
	for (const auto& c : clear_cases) {
		ds::frame_allocator_reset();
		single_camera camera(c.mode, false);
		opaque_pass pass;
		pass.execute(camera.context, camera.drv);
		const auto& desc = camera.drv.last_render_pass();
		BOOST_REQUIRE(desc.backbuffer);
		BOOST_REQUIRE_EQUAL(desc.colors.size(), 1u);
		BOOST_REQUIRE(desc.depth_stencil.has_value());
		BOOST_TEST((desc.colors[0].load == c.color));
		BOOST_TEST((desc.depth_stencil->load == c.depth));
		// whatever it clears, it clears only its own rectangle
		BOOST_TEST((desc.render_area == glm::ivec4{ 8, 16, 64, 32 }));
	}
}

BOOST_AUTO_TEST_CASE(OpaquePassClearsAnOffscreenTargetAsTheCameraAsks)
{
	for (const auto& c : clear_cases) {
		ds::frame_allocator_reset();
		single_camera camera(c.mode, true);
		opaque_pass pass;
		pass.execute(camera.context, camera.drv);
		const auto& desc = camera.drv.last_render_pass();
		BOOST_REQUIRE(!desc.backbuffer);
		BOOST_REQUIRE_EQUAL(desc.colors.size(), 1u);
		BOOST_TEST((desc.colors[0].load == c.color));
	}
}

BOOST_AUTO_TEST_CASE(ZPrepassClearsTheDepthUnlessTheCameraClearsNothing)
{
	for (const auto& c : clear_cases) {
		ds::frame_allocator_reset();
		single_camera camera(c.mode, false);
		z_prepass pass;
		pass.execute(camera.context, camera.drv);
		const auto& desc = camera.drv.last_render_pass();
		BOOST_REQUIRE(desc.depth_stencil.has_value());
		BOOST_TEST((desc.depth_stencil->load == c.depth));
	}
}

BOOST_AUTO_TEST_CASE(AnOverlayGroupKeepsTheColourOfTheGroupBelowItAndClearsOnlyItsDepth)
{
	ds::frame_allocator_reset();
	// extracted in the wrong order on purpose: the overlay (layer 1) first
	std::pmr::vector<render_packet_t> packets(ds::frame_allocator());
	packets.reserve(2);
	packets.push_back(make_packet(1, clear_mode::depth));
	packets.push_back(make_packet(0, clear_mode::all));
	sort_packets_by_priority(packets);

	driver::null::driver drv;
	test_util::test_render_context context;
	context.use_packets(&packets);
	z_prepass z;
	opaque_pass opaque;

	struct seen { driver::load_op depth; driver::load_op color; };
	std::vector<seen> groups;
	for_each_priority_group(packets, [&](std::span<render_packet_t> group) {
		context.use_group(group);
		z.execute(context, drv);
		const auto depth = drv.last_render_pass().depth_stencil->load;
		opaque.execute(context, drv);
		groups.push_back({ depth, drv.last_render_pass().colors[0].load });
	});
	context.use_all_packets();

	BOOST_REQUIRE_EQUAL(groups.size(), 2u);
	BOOST_TEST((groups[0].depth == driver::load_op::clear));
	BOOST_TEST((groups[0].color == driver::load_op::clear)); // the 3D world starts from a clean target
	BOOST_TEST((groups[1].depth == driver::load_op::clear)); // the overlay draws in front of it...
	BOOST_TEST((groups[1].color == driver::load_op::load));  // ...without wiping what the 3D world drew
	// one pass per group and per pass kind: nothing is drawn twice
	BOOST_TEST(drv.calls().render_pass_begins == 4u);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- the engine frame

namespace {

struct layers_fixture : test_util::null_engine_fixture
{
	scn::world_context& add_world(std::string_view name)
	{
		return storage.require<scn::context_manager>().create_context("level", name, scn::context_kind::level);
	}

	// Without a viewport and a target the camera draws to the whole backbuffer; with both it renders into a texture.
	static scn::camera_component& add_camera(scn::world_context& world, int layer,
		glm::ivec4 viewport = { 0, 0, 0, 0 }, const res::tag& target = {})
	{
		auto& reg = world.state();
		auto& cam = reg.emplace<scn::camera_component>(reg.create());
		cam.layer = layer;
		cam.m_viewport = scn::viewport{ viewport };
		if (target.is_valid()) {
			cam.color_targets.push_back(target);
		}
		return cam;
	}

	struct extracted
	{
		int priority = 0;
		clear_mode clear = clear_mode::all;
		glm::mat4 projection{ 1.0f };
		res::tag target;
		std::size_t opaque_draws = 0;
	};

	// What the extract stage made of the cameras, frame by frame.
	template <class OnFrame>
	void run_probed(int frames, OnFrame&& on_frame)
	{
		auto& stages = storage.require<app::frame_stage_registry>();
		stages.add_stage("camera_layers_probe", app::builtin_stage::extract, app::stage_side::after,
			[&](app::stage_context&) {
				std::vector<extracted> out;
				auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
				for (const auto& packet : packets) {
					out.push_back({ packet.camera.render_priority, packet.camera.clear, packet.camera.projection_matrix,
						rnd::select_camera_target(packet.camera).texture, packet.opaque_draws.size() });
				}
				on_frame(out);
			});
		BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(frames) == frames));
		stages.remove_stage("camera_layers_probe");
	}
};

// Logs what the packets of every call are: 'F' for a pass that runs once per frame, 'G' for a pass that runs per group.
struct recording_pass final : rnd::render_pass_interface
{
	using entry = std::pair<char, std::vector<int>>;

	recording_pass(bool once, std::vector<entry>& log) : m_once(once), m_log(log) {}

	void execute(rnd::render_context& context, rnd::driver::driver_interface&) override
	{
		m_log.push_back({ m_once ? 'F' : 'G', priorities_of(context.packets()) });
	}
	bool once_per_frame() const override { return m_once; }

private:
	bool m_once;
	std::vector<entry>& m_log;
};

}

BOOST_FIXTURE_TEST_SUITE(RndCameraLayersEngineTests, layers_fixture)

BOOST_AUTO_TEST_CASE(TheExtractorWritesLayerClearAndProjectionOfTheCamera)
{
	auto& world_3d = add_world("3d");
	auto& ui = add_world("ui");
	auto& cam_3d = add_camera(world_3d, 0, { 0, 0, 800, 400 }, res::tag(res::tag::memory, "__camera_layers_3d_rt"));
	cam_3d.fov = 60.f;
	auto& cam_ui = add_camera(ui, 1, { 0, 0, 800, 400 }, res::tag(res::tag::memory, "__camera_layers_ui_rt"));
	cam_ui.clear = scn::camera_clear::depth;
	cam_ui.projection = scn::camera_projection::ortho;
	cam_ui.ortho_size = 4.f;

	std::vector<extracted> seen;
	run_probed(1, [&](const std::vector<extracted>& packets) { seen = packets; });
	BOOST_REQUIRE_EQUAL(seen.size(), 2u);
	const auto by_priority = [&](int priority) -> const extracted& {
		return *std::ranges::find(seen, priority, &extracted::priority);
	};
	BOOST_REQUIRE(std::ranges::any_of(seen, [](const extracted& e) { return e.priority == 0; }));
	BOOST_REQUIRE(std::ranges::any_of(seen, [](const extracted& e) { return e.priority == 1; }));

	const auto& persp = by_priority(0);
	BOOST_TEST((persp.clear == clear_mode::all));
	BOOST_TEST(persp.projection[2][3] == -1.f); // perspective divides by the depth
	BOOST_TEST(persp.projection[3][3] == 0.f);
	BOOST_TEST(persp.projection[1][1] == 1.f / std::tan(glm::radians(60.f) * 0.5f), boost::test_tools::tolerance(1e-4f));

	const auto& ortho = by_priority(1);
	BOOST_TEST((ortho.clear == clear_mode::depth));
	BOOST_TEST(ortho.projection[2][3] == 0.f);
	BOOST_TEST(ortho.projection[3][3] == 1.f);
	// 4 world units high, 800x400: 8 units wide
	BOOST_TEST(ortho.projection[1][1] == 0.5f, boost::test_tools::tolerance(1e-5f));
	BOOST_TEST(ortho.projection[0][0] == 0.25f, boost::test_tools::tolerance(1e-5f));
}

BOOST_AUTO_TEST_CASE(RenderFrameDrawsOneGroupPerLayerLowestFirstAfterThePassesOfTheWholeFrame)
{
	// the overlay world is created first, so its cameras are extracted first
	auto& ui = add_world("ui");
	auto& world_3d = add_world("3d");
	add_camera(ui, 1);
	add_camera(ui, 1);
	add_camera(world_3d, 0);

	std::vector<recording_pass::entry> log;
	auto per_frame = std::make_shared<recording_pass>(true, log);
	auto per_group = std::make_shared<recording_pass>(false, log);
	auto& render = storage.require<rnd::render_system>();
	render.add_render_pass(per_frame);
	render.add_render_pass(per_group);
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(1) == 1));
	render.remove_render_pass(per_group);
	render.remove_render_pass(per_frame);

	BOOST_REQUIRE_EQUAL(log.size(), 3u);
	// the pass of the whole frame sees every packet, in render order
	BOOST_TEST(log[0].first == 'F');
	BOOST_TEST((log[0].second == std::vector<int>{ 0, 1, 1 }));
	// then layer 0 completely, then layer 1: each group holds only the packets of its layer
	BOOST_TEST(log[1].first == 'G');
	BOOST_TEST((log[1].second == std::vector<int>{ 0 }));
	BOOST_TEST(log[2].first == 'G');
	BOOST_TEST((log[2].second == std::vector<int>{ 1, 1 }));
}

BOOST_AUTO_TEST_CASE(TwoCamerasOfDifferentLayersShareOneTarget)
{
	const res::tag shared(res::tag::memory, "__camera_layers_shared_rt");
	auto& world_3d = add_world("3d");
	auto& ui = add_world("ui");
	add_camera(world_3d, 0, { 0, 0, 64, 64 }, shared);
	auto& cam_ui = add_camera(ui, 1, { 0, 0, 64, 64 }, shared);
	cam_ui.clear = scn::camera_clear::depth;
	cam_ui.projection = scn::camera_projection::ortho;

	std::vector<extracted> seen;
	run_probed(2, [&](const std::vector<extracted>& packets) { seen = packets; });
	BOOST_REQUIRE_EQUAL(seen.size(), 2u);
	BOOST_TEST((seen[0].target == shared));
	BOOST_TEST((seen[1].target == shared));

	// both groups drew into the one texture, which the first of them made and the second reused
	auto& textures = storage.require<rnd::render_system>().get_texture_manager();
	BOOST_TEST(textures.is_ready(textures.id_of(shared)));
	const vl::validator& state = validating_driver()->state();
	for (const auto& violation : state.violations()) {
		BOOST_ERROR("frame " << violation.frame << ", " << vl::rule_name(violation.kind) << ": " << violation.message);
	}
	BOOST_TEST(state.total() == 0u);
}

BOOST_AUTO_TEST_CASE(TheTwoWorldSmokeLevelDrawsBothLayersWithoutViolations, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_two_worlds.desc")));
	BOOST_REQUIRE_EQUAL(levels.level_set().size(), 2u);

	bool saw_3d = false;
	bool saw_overlay = false;
	run_probed(8, [&](const std::vector<extracted>& packets) {
		for (const auto& packet : packets) {
			if (packet.priority == 0) {
				saw_3d = saw_3d || packet.opaque_draws > 0;
				BOOST_TEST((packet.clear == clear_mode::all));
				BOOST_TEST(packet.projection[2][3] == -1.f);
			} else {
				BOOST_TEST(packet.priority == 1);
				saw_overlay = saw_overlay || packet.opaque_draws > 0;
				BOOST_TEST((packet.clear == clear_mode::depth));
				BOOST_TEST(packet.projection[3][3] == 1.f); // orthographic
			}
		}
	});
	BOOST_TEST(saw_3d);
	BOOST_TEST(saw_overlay);

	const vl::validator& state = validating_driver()->state();
	for (const auto& violation : state.violations()) {
		BOOST_ERROR("frame " << violation.frame << ", " << vl::rule_name(violation.kind) << ": " << violation.message);
	}
	BOOST_TEST(state.total() == 0u);
	BOOST_TEST(driver()->calls().draws > 0u);
}

BOOST_AUTO_TEST_SUITE_END()
