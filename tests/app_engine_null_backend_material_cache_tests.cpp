#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_model.h>
#include <algorithm>
#include <functional>
#include <set>
#include <vector>

// Extractor-level checks of the per-entity compiled material cache (US-45-3) on the null backend:
// the bench level has two cameras, so one extract stage visits every mesh twice.

namespace {

struct cached_entity
{
	entt::registry* reg = nullptr;
	entt::entity ent = entt::null;
	const rnd::compiled_material* material = nullptr;
};

struct material_cache_fixture : test_util::null_engine_fixture
{
	std::vector<cached_entity> snapshot()
	{
		std::vector<cached_entity> out;
		storage.require<scn::level_manager>().get_level().for_each_world([&out](scn::world& w) {
			auto& reg = w.state();
			for (auto [ent, cache] : reg.view<scn::compiled_material_component>().each()) {
				if (cache.material && cache.material->complete) {
					out.push_back({ &reg, ent, cache.material.get() });
				}
			}
		});
		return out;
	}

	std::set<const rnd::compiled_material*> current_materials()
	{
		std::set<const rnd::compiled_material*> out;
		storage.require<scn::level_manager>().get_level().for_each_world([&out](scn::world& w) {
			for (auto [ent, cache] : w.state().view<scn::compiled_material_component>().each()) {
				out.insert(cache.material.get());
			}
		});
		return out;
	}

	std::vector<const rnd::draw_call_t*> extracted_draws()
	{
		std::vector<const rnd::draw_call_t*> out;
		auto& packets = storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
		for (const auto& packet : packets) {
			for (const auto& dc : packet.opaque_draws) out.push_back(&dc);
			for (const auto& dc : packet.transparent_draws) out.push_back(&dc);
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
		auto& levels = storage.require<scn::level_manager>();
		BOOST_REQUIRE(levels.load(res::tag::make("levels/bench_100_meshes.desc")));
		BOOST_REQUIRE(levels.get_level().get_world_count() > 0u);
	}
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendMaterialCache, material_cache_fixture)

BOOST_AUTO_TEST_CASE(CleanFrameKeepsMaterialsAndDirtyOrReplacedDescRebuildsOnlyThatEntity)
{
	load_bench_level();

	std::vector<cached_entity> first;
	cached_entity dirtied;
	cached_entity replaced;
	const rnd::compiled_material* dirtied_before = nullptr;
	const rnd::compiled_material* replaced_before = nullptr;
	std::shared_ptr<scn::material_desc> donor_desc;

	run_scenario(
		{
			nullptr,
			nullptr,
			[&] {
				BOOST_REQUIRE(first.size() > 2u);
				dirtied = first.front();
				dirtied_before = dirtied.material;
				dirtied.reg->get<scn::material_desc_component>(dirtied.ent).dirty = true;
			},
			[&] {
				const auto desc_of = [](const cached_entity& e) { return e.reg->get<scn::material_desc_component>(e.ent).mlt_desc.get(); };
				const auto donor = std::ranges::find_if(first, [&](const cached_entity& e) { return desc_of(e) != desc_of(first.back()); });
				BOOST_REQUIRE_MESSAGE(donor != first.end(), "the bench level needs two different materials");
				replaced = first.back();
				replaced_before = replaced.material;
				donor_desc = desc_of(*donor);
				replaced.reg->get<scn::material_desc_component>(replaced.ent).mlt_desc = donor->reg->get<scn::material_desc_component>(donor->ent).mlt_desc;
			},
		},
		{
			[&] {
				first = snapshot();
				BOOST_REQUIRE(!first.empty());
			},
			[&] {
				for (const auto& e : first) {
					BOOST_TEST_CONTEXT("clean frame") {
						BOOST_CHECK(e.reg->get<scn::compiled_material_component>(e.ent).material.get() == e.material);
					}
				}
			},
			[&] {
				for (const auto& e : first) {
					const auto* now = e.reg->get<scn::compiled_material_component>(e.ent).material.get();
					if (e.ent == dirtied.ent && e.reg == dirtied.reg) {
						BOOST_CHECK(now != dirtied_before);
						BOOST_CHECK(!e.reg->get<scn::material_desc_component>(e.ent).dirty);
					} else {
						BOOST_CHECK(now == e.material);
					}
				}
			},
			[&] {
				const auto& cache = replaced.reg->get<scn::compiled_material_component>(replaced.ent);
				BOOST_CHECK(cache.material.get() != replaced_before);
				BOOST_CHECK(cache.material->source.get() == donor_desc.get());
			},
		});
}

BOOST_AUTO_TEST_CASE(DrawsOfAnEarlierCameraSurviveARebuildLaterInTheSameExtract)
{
	load_bench_level();

	std::set<const rnd::compiled_material*> drawn;

	run_scenario(
		{
			nullptr,
			[&] {
				// A sampler texture desc that never loads (no such file) leaves the material incomplete, so it is rebuilt on
				// every visit: the second camera replaces what the first camera's draws still point at.
				auto cached = snapshot();
				const auto first_drawn = std::ranges::find_if(cached, [&](const cached_entity& e) { return drawn.contains(e.material); });
				BOOST_REQUIRE_MESSAGE(first_drawn != cached.end(), "no entity of the bench level is drawn");
				auto& source = first_drawn->reg->get<scn::material_desc_component>(first_drawn->ent);
				const auto desc = source.mlt_desc.get();
				desc->samplers_textures.push_back(res::tag::make("textures/__missing_for_the_material_cache_test__.desc"));
				for (const auto& e : cached) {
					auto& mat = e.reg->get<scn::material_desc_component>(e.ent);
					if (mat.mlt_desc.get() == desc) mat.dirty = true;
				}
			},
		},
		{
			[&] {
				for (const auto* dc : extracted_draws()) drawn.insert(dc->material);
				BOOST_REQUIRE(!drawn.empty());
			},
			[&] {
				const auto live = current_materials();
				const auto draws = extracted_draws();
				BOOST_REQUIRE(!draws.empty());
				BOOST_REQUIRE_MESSAGE(std::ranges::any_of(live, [](const rnd::compiled_material* m) { return m && !m->complete; }),
					"the missing texture did not leave any material incomplete");

				std::size_t superseded = 0;
				for (const auto* dc : draws) {
					BOOST_REQUIRE(dc->material);
					BOOST_CHECK(dc->variant.is_valid());
					BOOST_CHECK(dc->material->base.is_valid());
					BOOST_CHECK(dc->material->source != nullptr);
					if (!live.contains(dc->material)) ++superseded;
				}
				BOOST_TEST(superseded > 0u);
			},
		});
}

BOOST_AUTO_TEST_SUITE_END()
