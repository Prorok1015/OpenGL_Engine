#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_model.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

// Extractor-level checks of the material instance cache (#125, US-45-3) on the null backend: the bench level has
// two cameras, so one extract stage visits every mesh twice.

namespace {

struct mesh_ref
{
	entt::registry* reg = nullptr;
	entt::entity ent = entt::null;
};

struct material_cache_fixture : test_util::null_engine_fixture
{
	rnd::material_instance_cache& instances() { return storage.require<rnd::render_system>().material_instances(); }

	// The first mesh entity of the level.
	mesh_ref first_mesh()
	{
		mesh_ref out;
		storage.require<scn::context_manager>().level_set().for_each([&out](scn::world_context& w) {
			if (out.reg) return;
			for (auto [ent, mat] : w.state().view<scn::material_desc_component>().each()) {
				if (mat.mlt_desc.get()) {
					out = { &w.state(), ent };
					return;
				}
			}
		});
		return out;
	}

	// The instances the frame's draws name, with their serial.
	std::map<std::uint32_t, std::uint64_t> drawn_instances()
	{
		std::map<std::uint32_t, std::uint64_t> out;
		auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
		for (const auto& packet : packets) {
			for (const auto* list : { &packet.opaque_draws, &packet.transparent_draws }) {
				for (const auto& dc : *list) {
					BOOST_REQUIRE(instances().contains(dc.material));
					out[dc.material.index] = instances().get(dc.material).serial;
				}
			}
		}
		return out;
	}

	// before[i] runs ahead of the extract stage of frame i, after[i] right behind it.
	void run_scenario(std::vector<std::function<void()>> before, std::vector<std::function<void()>> after)
	{
		auto& stages = storage.require<app::frame_stage_registry>();
		int before_frame = 0;
		int after_frame = 0;
		stages.add_stage("material_cache_before", app::builtin_stage::extract, app::stage_side::before,
			[&](app::stage_context&) {
				if (before_frame < static_cast<int>(before.size()) && before[before_frame]) before[before_frame]();
				++before_frame;
			});
		stages.add_stage("material_cache_after", app::builtin_stage::extract, app::stage_side::after,
			[&](app::stage_context&) {
				if (after_frame < static_cast<int>(after.size()) && after[after_frame]) after[after_frame]();
				++after_frame;
			});
		BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(static_cast<int>(std::max(before.size(), after.size()))) ==
			static_cast<int>(std::max(before.size(), after.size()))));
		stages.remove_stage("material_cache_before");
		stages.remove_stage("material_cache_after");
	}

	void load_bench_level()
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/bench_100_meshes.desc")));
		// The materials, and the textures they ask for from the first extract on, load in the background: let
		// them finish, so the scenario starts from compiled materials as it did when loads were synchronous.
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(3) == 3);
		BOOST_REQUIRE(levels.level_set().size() > 0u);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendMaterialCache, material_cache_fixture)

BOOST_AUTO_TEST_CASE(CleanFramesCompileNothingAndADirtyDescIsCompiledOncePerInstance)
{
	load_bench_level();

	std::vector<std::uint64_t> compilations;
	std::vector<std::map<std::uint32_t, std::uint64_t>> drawn;
	std::vector<std::size_t> alive;
	std::shared_ptr<scn::material_desc> edited;
	std::set<std::uint32_t> edited_ids;
	std::size_t alive_before_replace = 0;

	const auto record = [&] {
		compilations.push_back(instances().compilations());
		drawn.push_back(drawn_instances());
		alive.push_back(instances().size());
	};

	run_scenario(
		{
			nullptr, nullptr, nullptr,
			[&] {
				// An edit of a desc in place, marked on one mesh only.
				const mesh_ref mesh = first_mesh();
				BOOST_REQUIRE(mesh.reg);
				auto& mat = mesh.reg->get<scn::material_desc_component>(mesh.ent);
				edited = mat.mlt_desc.get();
				edited->shininess += 5.f;
				mat.dirty = true;
			},
			[&] {
				// A mesh gets another mesh's desc: its instance exists already, or is made once.
				const mesh_ref mesh = first_mesh();
				const auto own = mesh.reg->get<scn::material_desc_component>(mesh.ent).mlt_desc.get();
				bool found = false;
				for (auto [ent, mat] : mesh.reg->view<scn::material_desc_component>().each()) {
					const auto other = mat.mlt_desc.get();
					if (!found && other && other != own) {
						mesh.reg->get<scn::material_desc_component>(mesh.ent).mlt_desc = mat.mlt_desc;
						found = true;
					}
				}
				BOOST_REQUIRE_MESSAGE(found, "the bench level needs two different materials");
				alive_before_replace = instances().size();
			},
			nullptr,
		},
		{
			[&] { record(); },
			[&] { record(); },
			[&] { record(); },
			[&] {
				record();
				for (const auto& [id, serial] : drawn.back()) {
					if (instances().get(rnd::material_instance_id{ id }).source.get() == edited.get()) edited_ids.insert(id);
				}
			},
			[&] { record(); },
			[&] { record(); },
		});

	BOOST_REQUIRE(drawn.size() == 6u);
	BOOST_REQUIRE_MESSAGE(!drawn[0].empty(), "the bench level draws nothing");
	// A scene that does not change compiles nothing, however many meshes and cameras ask.
	BOOST_TEST(compilations[1] == compilations[0]);
	BOOST_TEST(compilations[2] == compilations[1]);
	BOOST_CHECK(drawn[1] == drawn[0]);
	BOOST_CHECK(drawn[2] == drawn[0]);

	// The edited desc: its instances (static and skinned) are compiled once each, in place; the others are untouched.
	BOOST_REQUIRE(!edited_ids.empty());
	BOOST_TEST(compilations[3] - compilations[2] == edited_ids.size());
	BOOST_TEST(compilations[3] - compilations[2] <= 2u);
	BOOST_TEST(drawn[3].size() == drawn[2].size());
	for (const auto& [id, serial] : drawn[3]) {
		BOOST_TEST_CONTEXT("instance " << id) {
			BOOST_REQUIRE(drawn[2].contains(id));
			BOOST_CHECK((serial != drawn[2].at(id)) == edited_ids.contains(id));
		}
	}

	// A mesh moved to another desc compiles at most the instance it did not have yet; the clean frame after it nothing.
	BOOST_TEST(compilations[4] - compilations[3] <= 1u);
	BOOST_TEST(compilations[5] == compilations[4]);
	BOOST_TEST(alive[4] <= alive_before_replace + 1u);
}

BOOST_AUTO_TEST_CASE(DrawsOfAnEarlierCameraStayValidWhenALaterVisitCompilesAgain)
{
	load_bench_level();

	std::vector<std::uint64_t> compilations;
	std::size_t incomplete_draws = 0;
	std::shared_ptr<scn::material_desc> broken;

	const auto check_draws = [&] {
		// Every draw of every camera names an instance that is in the cache, and it is the same instance for every
		// camera, however often it was compiled.
		auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
		BOOST_REQUIRE(!packets.empty());
		std::vector<std::set<std::uint32_t>> broken_ids;
		for (const auto& packet : packets) {
			std::set<std::uint32_t> ids;
			for (const auto* list : { &packet.opaque_draws, &packet.transparent_draws }) {
				const bool opaque_list = list == &packet.opaque_draws;
				for (const auto& dc : *list) {
					BOOST_REQUIRE(instances().contains(dc.material));
					const auto& material = instances().get(dc.material);
					BOOST_CHECK(dc.variant.is_valid());
					BOOST_CHECK(material.base.is_valid());
					// The compiles did not change the defines, so the variant of an earlier visit is the final one.
					BOOST_CHECK(dc.variant == (opaque_list ? material.opaque : material.base));
					if (broken && material.source.get() == broken.get()) {
						BOOST_CHECK(!material.complete);
						ids.insert(dc.material.index);
						++incomplete_draws;
					}
				}
			}
			broken_ids.push_back(std::move(ids));
		}
		for (const auto& ids : broken_ids) {
			BOOST_CHECK(ids == broken_ids.front());
		}
	};

	run_scenario(
		{
			nullptr,
			[&] {
				// A sampler texture desc that never loads (no such file) leaves the instances of the material incomplete, so each
				// frame compiles them again: once per instance, whatever the number of meshes and cameras that use the desc.
				const mesh_ref mesh = first_mesh();
				BOOST_REQUIRE(mesh.reg);
				auto& source = mesh.reg->get<scn::material_desc_component>(mesh.ent);
				broken = source.mlt_desc.get();
				broken->samplers_textures.push_back(res::tag::make("textures/__missing_for_the_material_cache_test__.desc"));
				source.dirty = true;
			},
			nullptr,
			nullptr,
		},
		{
			[&] { compilations.push_back(instances().compilations()); },
			[&] { compilations.push_back(instances().compilations()); check_draws(); },
			[&] { compilations.push_back(instances().compilations()); check_draws(); },
			[&] { compilations.push_back(instances().compilations()); check_draws(); },
		});

	BOOST_REQUIRE(compilations.size() == 4u);
	BOOST_TEST(incomplete_draws > 0u);
	// Frame 1 compiled the edited desc (static and skinned instance); frames 2 and 3 retry the incomplete ones only.
	for (std::size_t frame = 1; frame < compilations.size(); ++frame) {
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST(compilations[frame] - compilations[frame - 1] >= 1u);
			BOOST_TEST(compilations[frame] - compilations[frame - 1] <= 2u);
		}
	}
}

BOOST_AUTO_TEST_CASE(ADescEditedByTheLaterOfTwoMeshesReachesTheDrawsOfTheEarlierOne)
{
	load_bench_level();

	// Two meshes with one desc: whichever of them extraction visits first, the other one carries the dirty mark in
	// one of the two edit frames, so the instance is compiled again after draws of the desc were made.
	mesh_ref meshes[2];
	std::shared_ptr<scn::material_desc> shared;
	storage.require<scn::context_manager>().level_set().for_each([&](scn::world_context& w) {
		if (shared) return;
		std::map<const scn::material_desc*, std::vector<entt::entity>> by_desc;
		for (auto [ent, mat] : w.state().view<scn::material_desc_component>().each()) {
			if (const auto desc = mat.mlt_desc.get()) by_desc[desc.get()].push_back(ent);
		}
		for (auto& [desc, ents] : by_desc) {
			if (ents.size() >= 2) {
				shared = w.state().get<scn::material_desc_component>(ents.front()).mlt_desc.get();
				meshes[0] = { &w.state(), ents.front() };
				meshes[1] = { &w.state(), ents.back() };
				return;
			}
		}
	});
	BOOST_REQUIRE_MESSAGE(shared, "the bench level needs two meshes with one material");

	std::map<std::uint32_t, rnd::variant_id> before;	// opaque variant of the shared desc's instances, by id
	std::size_t checked = 0;
	std::size_t changed = 0;
	std::size_t older_hints = 0;

	const auto after_extract = [&] {
		auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
		std::map<std::uint32_t, rnd::variant_id> now;
		for (const auto& packet : packets) {
			for (const auto* list : { &packet.opaque_draws, &packet.transparent_draws }) {
				const bool opaque_list = list == &packet.opaque_draws;
				for (const auto& dc : *list) {
					const auto& material = instances().get(dc.material);
					if (material.source.get() != shared.get()) continue;
					++checked;
					// The variant stored in the draw may be older than the instance; the passes read the instance.
					if (dc.variant != (opaque_list ? material.opaque : material.base)) ++older_hints;
					if (opaque_list) {
						now[dc.material.index] = material.opaque;
						const auto old = before.find(dc.material.index);
						if (old != before.end() && old->second != material.opaque) ++changed;
					}
				}
			}
		}
		before = std::move(now);
	};

	const auto edit = [&](int mesh, const char* define) {
		return [&, mesh, define] {
			shared->defines.push_back(define);
			meshes[mesh].reg->get<scn::material_desc_component>(meshes[mesh].ent).dirty = true;
		};
	};

	run_scenario(
		{ nullptr, edit(1, "SNK_CACHE_TEST_FIRST"), nullptr, edit(0, "SNK_CACHE_TEST_SECOND"), nullptr },
		{ after_extract, after_extract, after_extract, after_extract, after_extract });

	BOOST_TEST(checked > 0u);
	// Each edit changed the variant of the instance. In one of the two orders a draw made before the recompile holds
	// the older variant: that is why the passes take the program from the instance and not from the draw.
	BOOST_TEST(changed > 0u);
	BOOST_TEST(older_hints > 0u);
	BOOST_TEST_MESSAGE("draws of the shared desc " << checked << ", with an older variant in the draw " << older_hints);
}

BOOST_AUTO_TEST_SUITE_END()
