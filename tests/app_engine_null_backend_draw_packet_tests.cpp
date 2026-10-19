#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_model.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// The extractor's POD draw packets (R6a, #124) on the null backend: sorting reorders the draws of a packet but
// neither adds nor drops one, and every draw still carries the object data of its mesh.

namespace {

using transform_bytes = std::array<float, 16>;

transform_bytes bytes_of(const glm::mat4& m)
{
	transform_bytes out;
	std::memcpy(out.data(), &m, sizeof(m));
	return out;
}

// What the packets of one world should hold, from the ECS.
struct expected_world
{
	std::vector<transform_bytes> opaque;
	std::vector<transform_bytes> transparent;
};

struct draw_packet_fixture : test_util::null_engine_fixture
{
	std::vector<expected_world> expected()
	{
		std::vector<expected_world> out;
		storage.require<scn::context_manager>().level_set().for_each([&out](scn::world_context& w) {
			if (!w.extract()) return;
			expected_world world;
			auto view = w.state().view<scn::name_component, scn::mesh_component, scn::geometry_component,
				scn::material_desc_component, scn::world_transform>();
			for (auto&& [ent, name, mesh, geom, mat, trans] : view.each()) {
				if (!mat.mlt_desc) continue;
				const auto queue = mat.mlt_desc->queue;
				if (queue == scn::pass_queue::OPAQUE || queue == scn::pass_queue::MIX) world.opaque.push_back(bytes_of(trans.world));
				if (queue == scn::pass_queue::TRANSPARENT || queue == scn::pass_queue::MIX) world.transparent.push_back(bytes_of(trans.world));
			}
			std::ranges::sort(world.opaque);
			std::ranges::sort(world.transparent);
			out.push_back(std::move(world));
		});
		return out;
	}

	// Calls `probe` once per frame right behind the extract stage, before the passes consume the packets.
	template <class Probe>
	void run_probed(int frames, Probe&& probe)
	{
		auto& stages = storage.require<app::frame_stage_registry>();
		stages.add_stage("draw_packet_probe", app::builtin_stage::extract, app::stage_side::after,
			[&](app::stage_context&) { probe(); });
		BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(frames) == frames));
		stages.remove_stage("draw_packet_probe");
	}

	std::pmr::vector<rnd::render_packet_t>& packets()
	{
		return storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
	}

	void load_bench_level()
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/bench_100_meshes.desc")));
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(3) == 3);
		BOOST_REQUIRE(levels.level_set().size() > 0u);
	}
};

std::vector<transform_bytes> transforms_of(const std::pmr::vector<rnd::draw_call_t>& draws)
{
	std::vector<transform_bytes> out;
	out.reserve(draws.size());
	for (const auto& dc : draws) out.push_back(bytes_of(dc.transform));
	std::ranges::sort(out);
	return out;
}

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendDrawPacket, draw_packet_fixture)

BOOST_AUTO_TEST_CASE(SortedPacketsDrawTheSameMeshesAsTheScene)
{
	load_bench_level();
	const auto worlds = expected();
	BOOST_REQUIRE(!worlds.empty());

	std::size_t checked_packets = 0;
	std::size_t checked_draws = 0;
	run_probed(3, [&] {
		for (const auto& packet : packets()) {
			const auto opaque = transforms_of(packet.opaque_draws);
			const auto transparent = transforms_of(packet.transparent_draws);
			// The packet holds exactly the draws one of the worlds asks for, in whatever order.
			const bool matches_a_world = std::ranges::any_of(worlds, [&](const expected_world& w) {
				return w.opaque == opaque && w.transparent == transparent;
			});
			BOOST_CHECK_MESSAGE(matches_a_world, "a packet's draws are not the scene's meshes");
			++checked_packets;
			checked_draws += packet.opaque_draws.size() + packet.transparent_draws.size();

			BOOST_CHECK(std::ranges::is_sorted(packet.opaque_draws, {}, &rnd::draw_call_t::sort_key));
			BOOST_CHECK(std::ranges::is_sorted(packet.transparent_draws, {}, &rnd::draw_call_t::sort_key));
		}
	});
	BOOST_TEST(checked_packets > 0u);
	BOOST_TEST(checked_draws > 0u);
	BOOST_TEST_MESSAGE("packets checked " << checked_packets << ", draws " << checked_draws);
}

BOOST_AUTO_TEST_CASE(DrawIdsResolveInsideTheirPacket)
{
	load_bench_level();

	std::size_t skinned = 0;
	std::size_t checked = 0;
	run_probed(2, [&] {
		for (const auto& packet : packets()) {
			const auto check = [&](const rnd::draw_call_t& dc) {
				++checked;
				BOOST_REQUIRE(dc.geometry.is_valid());
				BOOST_REQUIRE(dc.geometry.index < packet.geometries.size());
				BOOST_CHECK(packet.geometry_of(dc).is_valid());
				BOOST_REQUIRE(dc.material.is_valid());
				BOOST_REQUIRE(dc.material.index < packet.materials.size());
				BOOST_CHECK(packet.materials[dc.material.index] != nullptr);
				BOOST_CHECK(dc.variant.is_valid());
				// The bone range of a skinned draw lies inside the packet's palette.
				BOOST_CHECK(!dc.skin.is_valid() || dc.bone_count > 0u);
				if (dc.skin.is_valid()) {
					++skinned;
					BOOST_REQUIRE(dc.skin.index < packet.skins.size());
					BOOST_CHECK(packet.skin_of(dc).is_valid());
					BOOST_CHECK(dc.bone_offset + dc.bone_count <= packet.bones.matrices.size());
				}
			};
			for (const auto& dc : packet.opaque_draws) check(dc);
			for (const auto& dc : packet.transparent_draws) check(dc);

			// Each distinct geometry and material appears once in its table.
			auto geometries = std::vector<std::string>();
			for (const auto& tag : packet.geometries) geometries.push_back(tag.string());
			std::ranges::sort(geometries);
			BOOST_CHECK(std::ranges::adjacent_find(geometries) == geometries.end());
			auto materials = std::vector<const rnd::compiled_material*>(packet.materials.begin(), packet.materials.end());
			std::ranges::sort(materials);
			BOOST_CHECK(std::ranges::adjacent_find(materials) == materials.end());
		}
	});
	BOOST_TEST(checked > 0u);
	BOOST_TEST_MESSAGE("draws checked " << checked << ", skinned " << skinned);
}

BOOST_AUTO_TEST_CASE(TransparentDrawsComeFarthestFirst)
{
	load_bench_level();

	std::size_t checked = 0;
	run_probed(2, [&] {
		for (const auto& packet : packets()) {
			std::uint32_t previous = 0;
			bool first = true;
			for (const auto& dc : packet.transparent_draws) {
				const glm::vec4 view_pos = packet.camera.view_matrix * glm::vec4(glm::vec3(dc.transform[3]), 1.0f);
				const std::uint32_t depth = rnd::quantize_view_depth(-view_pos.z);
				if (!first) {
					BOOST_CHECK_MESSAGE(depth <= previous, "a transparent draw is nearer than the one before");
				}
				previous = depth;
				first = false;
				++checked;
			}
		}
	});
	BOOST_TEST_MESSAGE("transparent draws checked " << checked);
}

BOOST_AUTO_TEST_SUITE_END()
