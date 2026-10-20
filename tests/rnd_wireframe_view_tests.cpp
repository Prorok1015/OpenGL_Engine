#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "rnd_composition_pass.h"
#include "rnd_null_driver.h"
#include "rnd_opaque_pass.h"
#include "rnd_skybox_pass.h"
#include "rnd_transparent_pass.h"
#include "rnd_z_prepass.h"
#include <snk/application/app_frame_stage.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/level/scn_context_manager.h>
#include <snk/scene/scn_camera_component.hpp>
#include <memory>
#include <memory_resource>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// The wireframe view (#352) is a flag of one camera: the scene passes draw its geometry as lines, the sky, the
// composition and the debug lines stay filled, and the other cameras of the frame are not touched. Runs on the
// null backend, which counts the draws per polygon mode.

namespace {

using namespace rnd;
using driver::polygon_mode;

// What a draw of every kind needs from the renderer, served by the null driver.
class drawing_context final : public render_context
{
public:
	explicit drawing_context(driver::null::driver& drv)
	{
		m_texture = drv.create_texture(driver::texture_header{});
		m_vertex_array = drv.create_vertex_array();
		m_program = drv.create_shader({
			{ "a.vert", "#version 450 core\nvoid main() {}\n", driver::shader_header::shader_type::VERTEX },
			{ "a.frag", "#version 450 core\nvoid main() {}\n", driver::shader_header::shader_type::FRAGMENT },
		});
		const auto layout = drv.create_bind_group_layout({});
		m_layouts = { layout, layout, layout, layout, layout };

		shader_config config;
		m_material.base = m_material.opaque = m_variants.intern(config.cdata);
		m_material.runtime.samplers.push_back(texture_id{ 1 });
	}

	void use_packets(std::pmr::vector<render_packet_t>* packets) { m_packets = packets; }

	std::span<render_packet_t> packets() override { return m_packets ? std::span<render_packet_t>{ *m_packets } : std::span<render_packet_t>{}; }
	driver::texture_handle find_texture(const res::tag&) const override { return m_texture; }
	driver::texture_handle texture(texture_id) const override { return m_texture; }
	driver::texture_handle ensure_render_target(const res::tag&, glm::ivec2, driver::texture_header::TYPE,
		driver::texture_usage, const render_targets::sampling&) override { return m_texture; }
	driver::vertex_array_interface* geometry(const res::tag&) override { return m_vertex_array.get(); }
	shader_config_cache& variants() override { return m_variants; }
	driver::shader_interface* program(variant_id) override { return m_program.get(); }
	const set_layouts& layouts() const override { return m_layouts; }
	void write_view(const global_params&) override {}
	void write_lights(const lights_params&) override {}
	void bind_frame() override {}
	const compiled_material& material(material_instance_id) const override { return m_material; }
	void bind_material(material_instance_id) override {}
	void upload_objects(std::span<const object_data>, std::span<const glm::mat4>) override {}
	void bind_object(std::uint32_t, const res::tag&) override {}

	variant_id variant() const { return m_material.base; }

private:
	std::pmr::vector<render_packet_t>* m_packets = nullptr;
	driver::texture_handle m_texture;
	std::unique_ptr<driver::vertex_array_interface> m_vertex_array;
	std::unique_ptr<driver::shader_interface> m_program;
	set_layouts m_layouts;
	shader_config_cache m_variants;
	compiled_material m_material;
};

constexpr std::size_t opaque_draws = 2;
constexpr std::size_t transparent_draws = 1;

// A camera that draws into its own texture: two opaque draws, a transparent one and a sky.
render_packet_t make_packet(const drawing_context& context, const char* target, bool wireframe)
{
	render_packet_t packet;
	packet.camera.viewport = { 0, 0, 64, 64 };
	packet.camera.color_targets.push_back(res::tag(res::tag::memory, target));
	packet.camera.wireframe = wireframe;
	packet.geometries.push_back(res::tag(res::tag::memory, "wireframe_view_geometry"));
	draw_call_t draw;
	draw.geometry.index = 0;
	draw.variant = context.variant();
	draw.material.index = 0;
	draw.indices_count = 3;
	for (std::size_t i = 0; i < opaque_draws; ++i) {
		packet.opaque_draws.push_back(draw);
	}
	for (std::size_t i = 0; i < transparent_draws; ++i) {
		packet.transparent_draws.push_back(draw);
	}
	packet.skybox_material = skybox_draw_t{ context.variant(), material_instance_id{ 0 }, std::nullopt };
	return packet;
}

struct polygon_draws
{
	std::size_t lines = 0;
	std::size_t fills = 0;
	bool operator==(const polygon_draws&) const = default;
	friend std::ostream& operator<<(std::ostream& out, const polygon_draws& d) { return out << "{lines " << d.lines << ", fills " << d.fills << "}"; }
};

// The draws one pass made over the packets of the context.
polygon_draws run(render_pass_interface& pass, drawing_context& context, driver::null::driver& drv)
{
	const std::size_t lines = drv.draws_with(polygon_mode::LINE);
	const std::size_t fills = drv.draws_with(polygon_mode::FILL);
	pass.execute(context, drv);
	return { drv.draws_with(polygon_mode::LINE) - lines, drv.draws_with(polygon_mode::FILL) - fills };
}

struct pass_draws
{
	polygon_draws z, opaque, sky, transparent, composition;
};

// Every pass over the cameras, one pass after the other as the frame runs them.
pass_draws run_scene(const std::vector<bool>& wireframe_of_cameras)
{
	ds::frame_allocator_reset();
	driver::null::driver drv;
	drawing_context context(drv);
	std::pmr::vector<render_packet_t> packets(ds::frame_allocator());
	packets.reserve(wireframe_of_cameras.size());
	int camera = 0;
	for (const bool wireframe : wireframe_of_cameras) {
		const std::string target = "__wireframe_view_rt_" + std::to_string(camera++);
		packets.push_back(make_packet(context, target.c_str(), wireframe));
	}
	context.use_packets(&packets);

	z_prepass z;
	opaque_pass opaque;
	skybox_pass sky;
	transparent_pass transparent;
	composition_pass composition;
	pass_draws out;
	out.z = run(z, context, drv);
	out.opaque = run(opaque, context, drv);
	out.sky = run(sky, context, drv);
	out.transparent = run(transparent, context, drv);
	out.composition = run(composition, context, drv);
	return out;
}

}

BOOST_AUTO_TEST_SUITE(RndWireframeViewTests)

BOOST_AUTO_TEST_CASE(ACameraWithoutTheFlagDrawsEverythingFilled)
{
	BOOST_TEST(camera_render_data_t{}.wireframe == false);

	const pass_draws draws = run_scene({ false });
	BOOST_TEST(draws.z == (polygon_draws{ 0, opaque_draws }));
	BOOST_TEST(draws.opaque == (polygon_draws{ 0, opaque_draws }));
	BOOST_TEST(draws.sky == (polygon_draws{ 0, 1 }));
	BOOST_TEST(draws.transparent == (polygon_draws{ 0, transparent_draws }));
	BOOST_TEST(draws.composition == (polygon_draws{ 0, 1 }));
}

BOOST_AUTO_TEST_CASE(ACameraWithTheFlagDrawsTheSceneAsLinesAndTheRestFilled)
{
	const pass_draws draws = run_scene({ true });
	BOOST_TEST(draws.z == (polygon_draws{ opaque_draws, 0 }));
	BOOST_TEST(draws.opaque == (polygon_draws{ opaque_draws, 0 }));
	BOOST_TEST(draws.transparent == (polygon_draws{ transparent_draws, 0 }));
	// the sky and the composition of the same camera are not wireframe
	BOOST_TEST(draws.sky == (polygon_draws{ 0, 1 }));
	BOOST_TEST(draws.composition == (polygon_draws{ 0, 1 }));
}

BOOST_AUTO_TEST_CASE(TheWireframeOfOneCameraDoesNotReachTheOtherCamerasOfTheFrame)
{
	// the editor camera first, then a game camera
	const pass_draws draws = run_scene({ true, false });
	BOOST_TEST(draws.z == (polygon_draws{ opaque_draws, opaque_draws }));
	BOOST_TEST(draws.opaque == (polygon_draws{ opaque_draws, opaque_draws }));
	BOOST_TEST(draws.transparent == (polygon_draws{ transparent_draws, transparent_draws }));
	BOOST_TEST(draws.sky == (polygon_draws{ 0, 2 }));
	BOOST_TEST(draws.composition == (polygon_draws{ 0, 2 }));

	// the order of the cameras does not matter
	const pass_draws reversed = run_scene({ false, true });
	BOOST_TEST(reversed.opaque == draws.opaque);
	BOOST_TEST(reversed.transparent == draws.transparent);
	BOOST_TEST(reversed.sky == draws.sky);
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------- the extractor and the engine frame

BOOST_FIXTURE_TEST_SUITE(RndWireframeViewEngineTests, test_util::null_engine_fixture)

BOOST_AUTO_TEST_CASE(TheCameraComponentFlagReachesItsPacketOnly)
{
	auto& world = storage.require<scn::context_manager>().create_context("level", "wireframe", scn::context_kind::level);
	auto& reg = world.state();
	const auto add_camera = [&](const char* target, bool wireframe) {
		auto& cam = reg.emplace<scn::camera_component>(reg.create());
		cam.m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 64, 64 } };
		cam.color_targets.push_back(res::tag(res::tag::memory, target));
		cam.wireframe = wireframe;
	};
	add_camera("__wireframe_view_editor_rt", true);
	add_camera("__wireframe_view_game_rt", false);

	std::size_t wire = 0;
	std::size_t filled = 0;
	auto& stages = storage.require<app::frame_stage_registry>();
	stages.add_stage("wireframe_probe", app::builtin_stage::extract, app::stage_side::after, [&](app::stage_context&) {
		auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
		for (const auto& packet : packets) {
			const bool editor = rnd::select_camera_target(packet.camera).texture == res::tag(res::tag::memory, "__wireframe_view_editor_rt");
			BOOST_TEST(packet.camera.wireframe == editor);
			(packet.camera.wireframe ? wire : filled) += 1;
		}
	});
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(1) == 1));
	stages.remove_stage("wireframe_probe");
	BOOST_TEST(wire == 1u);
	BOOST_TEST(filled == 1u);
}

BOOST_AUTO_TEST_CASE(TheWireframeCameraOfTheSmokeLevelTurnsItsSceneDrawsIntoLinesAndNothingElse, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	auto& levels = storage.require<scn::context_manager>();
	BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));

	// a second camera next to the level's own (which draws to the backbuffer and is never wireframe)
	entt::registry* camera_registry = nullptr;
	entt::entity camera_entity = entt::null;
	levels.level_set().for_each([&](scn::world_context& world) {
		if (camera_registry) return;
		auto& reg = world.state();
		camera_entity = reg.create();
		auto& cam = reg.emplace<scn::camera_component>(camera_entity);
		cam.m_viewport = scn::viewport{ glm::ivec4{ 0, 0, 64, 64 } };
		cam.color_targets.push_back(res::tag(res::tag::memory, "__wireframe_view_smoke_rt"));
		cam.depth_target = res::tag(res::tag::memory, "__wireframe_view_smoke_depth_rt");
		cam.tp_accum_target = res::tag(res::tag::memory, "__wireframe_view_smoke_accum_rt");
		cam.tp_reveal_target = res::tag(res::tag::memory, "__wireframe_view_smoke_reveal_rt");
		camera_registry = &reg;
	});
	BOOST_REQUIRE(camera_registry);

	constexpr int warm_up = 12;
	constexpr int frames = 20;
	std::vector<polygon_draws> per_frame;
	std::size_t lines = 0;
	std::size_t fills = 0;
	auto& stages = storage.require<app::frame_stage_registry>();
	// the flag is up from the frame `warm_up` on (the camera component lives in the registry of the world)
	stages.add_stage("wireframe_toggle", app::builtin_stage::extract, app::stage_side::before, [&](app::stage_context&) {
		camera_registry->get<scn::camera_component>(camera_entity).wireframe = per_frame.size() >= static_cast<std::size_t>(warm_up);
	});
	stages.add_stage("wireframe_counts", app::builtin_stage::present, app::stage_side::after, [&](app::stage_context&) {
		const std::size_t now_lines = driver()->draws_with(polygon_mode::LINE);
		const std::size_t now_fills = driver()->draws_with(polygon_mode::FILL);
		per_frame.push_back({ now_lines - lines, now_fills - fills });
		lines = now_lines;
		fills = now_fills;
	});
	BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(frames) == frames));
	stages.remove_stage("wireframe_counts");
	stages.remove_stage("wireframe_toggle");

	BOOST_REQUIRE_EQUAL(per_frame.size(), static_cast<std::size_t>(frames));
	const polygon_draws filled = per_frame[warm_up - 1];
	BOOST_TEST(filled.fills > 0u);
	for (int frame = 0; frame < warm_up; ++frame) {
		BOOST_TEST(per_frame[frame].lines == 0u, "frame " << frame << " drew lines without the flag");
	}
	for (int frame = warm_up + 3; frame < frames; ++frame) {
		const polygon_draws& wire = per_frame[frame];
		BOOST_TEST(wire.lines > 0u, "frame " << frame);
		// the same draws as before, some of them as lines: the level's own camera and the sky stay filled
		BOOST_TEST(wire.lines + wire.fills == filled.fills, "frame " << frame);
		BOOST_TEST(wire.fills > 0u, "frame " << frame);
		BOOST_TEST(wire.fills < filled.fills, "frame " << frame);
	}
}

BOOST_AUTO_TEST_SUITE_END()
