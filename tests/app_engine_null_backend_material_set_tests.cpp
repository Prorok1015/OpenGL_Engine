#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_model.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <set>
#include <type_traits>
#include <vector>

// Set 2 on the whole engine frame (null backend, validation on): every draw of a material has the group of its
// material bound, the group holds what the material compiler produced, nothing is rebuilt while the scene stays
// the same, and a material that is marked dirty or whose texture is reloaded gets a group of its own.

namespace vl = rnd::driver::validation;
using namespace rnd::driver;

namespace {

struct material_set_fixture : test_util::null_engine_fixture
{
	void load_warm_smoke_level()
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(3) == 3);
	}

	// The packets of the frame being rendered: only there while a frame is in progress.
	std::pmr::vector<rnd::render_packet_t>& packets()
	{
		return storage.require<rnd::frame_context>().data.require<std::pmr::vector<rnd::render_packet_t>>();
	}

	// Calls `before_extract(i)` ahead of the extract stage of frame i and `after_render(i)` right after the passes of it.
	void run_with(int frames, std::function<void(int)> before_extract, std::function<void(int)> after_render)
	{
		auto& stages = storage.require<app::frame_stage_registry>();
		int before_frame = 0;
		int after_frame = 0;
		stages.add_stage("material_set_before", app::builtin_stage::extract, app::stage_side::before,
			[&](app::stage_context&) { if (before_extract) before_extract(before_frame); ++before_frame; });
		stages.add_stage("material_set_after", app::builtin_stage::render, app::stage_side::after,
			[&](app::stage_context&) { if (after_render) after_render(after_frame); ++after_frame; });
		BOOST_REQUIRE_NO_THROW(BOOST_TEST(run_frames(frames) == frames));
		stages.remove_stage("material_set_before");
		stages.remove_stage("material_set_after");
	}

	void expect_no_violations()
	{
		const vl::validator& state = validating_driver()->state();
		for (const auto& violation : state.violations()) {
			BOOST_ERROR("frame " << violation.frame << ", " << vl::rule_name(violation.kind) << ": " << violation.message);
		}
		BOOST_TEST(state.total() == 0u);
	}

	// The compiled materials the frame's draws point at.
	std::set<const rnd::compiled_material*> drawn_materials()
	{
		std::set<const rnd::compiled_material*> result;
		for (const auto& packet : packets()) {
			for (const auto& dc : packet.opaque_draws) result.insert(dc.material);
			for (const auto& dc : packet.transparent_draws) result.insert(dc.material);
		}
		return result;
	}

	// The groups bound at set 2 by the log entries from `first` on.
	std::set<std::uint64_t> material_groups_bound(std::size_t first)
	{
		std::set<std::uint64_t> result;
		const auto& log = driver()->bind_log();
		for (std::size_t i = first; i < log.size(); ++i) {
			if (log[i].set == material_set) {
				result.insert((std::uint64_t{ log[i].group.gen } << 32) | log[i].group.index);
			}
		}
		return result;
	}

	rnd::texture_manager& textures() { return storage.require<rnd::render_system>().get_texture_manager(); }
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendMaterialSet, material_set_fixture)

BOOST_AUTO_TEST_CASE(EveryDrawOfAMaterialHasItsGroupBound)
{
	load_warm_smoke_level();
	const auto before = driver()->draws_with_set();
	BOOST_REQUIRE(run_frames(4) == 4);

	const auto with_set = [&](std::uint32_t set) { return driver()->draws_with_set()[set] - before[set]; };
	BOOST_TEST(with_set(material_set) > 0u);
	// The scene shaders read the material and the object together: a draw has both or neither, the sky and the
	// composition have neither.
	BOOST_TEST(with_set(material_set) == with_set(object_set));
	// What the validation checks against the pipeline's layout: a group at set 2 of the layout the pipeline names.
	const vl::validator& state = validating_driver()->state();
	BOOST_TEST(state.count(vl::rule::draw_missing_bind_group) == 0u);
	BOOST_TEST(state.count(vl::rule::bind_group_layout_mismatch) == 0u);
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(TheGroupHoldsWhatTheCompilerProducedForTheMaterial)
{
	load_warm_smoke_level();
	driver()->record_bindings(true);
	std::size_t log_begin = driver()->bind_log().size();
	std::size_t frames_checked = 0;
	std::size_t textured = 0;

	run_with(3, nullptr, [&](int) {
		const auto materials = drawn_materials();
		BOOST_REQUIRE(!materials.empty());
		const auto groups = material_groups_bound(log_begin);
		// One group per compiled material, whatever the number of draws, passes and cameras that use it.
		BOOST_TEST(groups.size() == materials.size());

		std::size_t matched = 0;
		for (std::size_t i = log_begin; i < driver()->bind_log().size(); ++i) {
			const auto& bind = driver()->bind_log()[i];
			if (bind.set != material_set) {
				continue;
			}
			const auto* info = driver()->group_info(bind.group);
			BOOST_REQUIRE(info);
			BOOST_REQUIRE(info->entries.size() == 4u);
			const auto* bytes = driver()->buffer_bytes(info->entries[0].buffer);
			BOOST_REQUIRE(bytes);
			BOOST_REQUIRE(bytes->size() == sizeof(rnd::material_params));
			const bool known = std::ranges::any_of(materials, [&](const rnd::compiled_material* m) {
				return m->runtime.material_params.size() == bytes->size()
					&& std::memcmp(m->runtime.material_params.data(), bytes->data(), bytes->size()) == 0;
			});
			BOOST_CHECK(known);
			matched += known ? 1 : 0;
		}
		BOOST_TEST(matched > 0u);

		// A material with a diffuse map is drawn with that map at slot 0 (the validation checks it is alive and sampled).
		for (const auto* material : materials) {
			if (material->runtime.samplers.empty() || !material->runtime.samplers[0]) {
				continue;
			}
			++textured;
			const rnd::driver::texture_interface* diffuse = material->runtime.samplers[0]->native();
			bool found = false;
			for (std::size_t i = log_begin; i < driver()->bind_log().size() && !found; ++i) {
				const auto& bind = driver()->bind_log()[i];
				const auto* info = bind.set == material_set ? driver()->group_info(bind.group) : nullptr;
				found = info && info->entries[1].texture == diffuse;
			}
			BOOST_CHECK(found);
		}
		log_begin = driver()->bind_log().size();
		++frames_checked;
	});
	BOOST_TEST(frames_checked == 3u);
	BOOST_TEST(textured > 0u);
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(NoGroupIsRebuiltOnceTheSceneIsWarm)
{
	load_warm_smoke_level();
	driver()->record_bindings(true);
	std::size_t log_begin = driver()->bind_log().size();
	std::vector<std::set<std::uint64_t>> bound;
	std::vector<std::size_t> created;
	std::vector<std::size_t> buffers;
	std::vector<std::size_t> live;
	run_with(8, nullptr, [&](int) {
		bound.push_back(material_groups_bound(log_begin));
		log_begin = driver()->bind_log().size();
		created.push_back(driver()->calls().bind_groups_created);
		buffers.push_back(driver()->calls().buffers_created);
		live.push_back(driver()->live_bind_groups());
	});

	BOOST_REQUIRE(bound.size() == 8u);
	BOOST_REQUIRE(!bound[0].empty());
	for (std::size_t frame = 1; frame < bound.size(); ++frame) {
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST((bound[frame] == bound[0]));	// the very same groups, frame after frame
			BOOST_TEST(created[frame] == created[0]);
			BOOST_TEST(buffers[frame] == buffers[0]);
			BOOST_TEST(live[frame] == live[0]);
		}
	}
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(AMaterialMarkedDirtyGetsOneNewGroupAndTheOldOneGoesAway)
{
	load_warm_smoke_level();
	driver()->record_bindings(true);

	entt::registry* registry = nullptr;
	entt::entity entity = entt::null;
	const rnd::compiled_material* dirtied_before = nullptr;
	std::vector<std::set<std::uint64_t>> bound;
	std::vector<std::size_t> created;
	std::vector<std::size_t> buffers;
	std::vector<std::size_t> live;
	std::size_t log_begin = driver()->bind_log().size();

	run_with(6,
		[&](int frame) {
			if (frame == 2) {
				BOOST_REQUIRE(registry);
				registry->get<scn::material_desc_component>(entity).dirty = true;
			}
		},
		[&](int frame) {
			if (frame == 1) {
				// A material that is drawn (the sky's is not): the one to dirty.
				const auto drawn = drawn_materials();
				storage.require<scn::context_manager>().get_level().for_each_world([&](scn::world& w) {
					if (registry) return;
					for (auto [ent, cache] : w.state().view<scn::compiled_material_component>().each()) {
						if (cache.material && drawn.contains(cache.material.get())) {
							registry = &w.state();
							entity = ent;
							dirtied_before = cache.material.get();
							return;
						}
					}
				});
				BOOST_REQUIRE(registry);
			}
			bound.push_back(material_groups_bound(log_begin));
			log_begin = driver()->bind_log().size();
			created.push_back(driver()->calls().bind_groups_created);
			buffers.push_back(driver()->calls().buffers_created);
			live.push_back(driver()->live_bind_groups());
		});

	BOOST_REQUIRE(bound.size() == 6u);
	BOOST_TEST((registry->get<scn::compiled_material_component>(entity).material.get() != dirtied_before));
	// Frame 2 built the dirtied material again: one group and one params buffer, nothing else.
	BOOST_TEST(created[2] == created[1] + 1);
	BOOST_TEST(buffers[2] == buffers[1] + 1);
	BOOST_TEST(bound[2].size() == bound[1].size());
	std::set<std::uint64_t> replaced;
	std::ranges::set_difference(bound[2], bound[1], std::inserter(replaced, replaced.end()));
	BOOST_TEST(replaced.size() == 1u);
	// Later frames keep to the new group, and the group of the material it replaced is not kept.
	for (std::size_t frame = 3; frame < bound.size(); ++frame) {
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST((bound[frame] == bound[2]));
			BOOST_TEST(created[frame] == created[2]);
		}
	}
	BOOST_TEST(live[5] == live[1]);
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(ReloadingATextureBuildsTheGroupsAgainWithTheNewTexture)
{
	load_warm_smoke_level();
	driver()->record_bindings(true);

	res::tag reloaded;
	std::size_t log_begin = driver()->bind_log().size();
	std::size_t created_before = 0;
	std::size_t live_before = 0;
	std::size_t created_after = 0;
	std::size_t live_after = 0;
	std::size_t groups_with_the_texture = 0;
	const rnd::driver::texture_interface* new_texture = nullptr;

	run_with(10,
		[&](int frame) {
			if (frame == 2) {
				// What the editor does when the picture file changes on disk.
				BOOST_REQUIRE(reloaded.is_valid());
				textures().remove(reloaded);
			}
			test_util::pump(tasks);
		},
		[&](int frame) {
			if (frame == 1) {
				// A texture of a drawn material, by the tag its description names.
				for (const auto* material : drawn_materials()) {
					if (!material->runtime.samplers.empty() && material->runtime.samplers[0]) {
						reloaded = static_cast<const scn::material_desc*>(material->source.get())->samplers_textures[0];
						break;
					}
				}
				BOOST_REQUIRE_MESSAGE(reloaded.is_valid(), "no drawn material of the smoke level has a diffuse map");
				BOOST_REQUIRE(textures().find(reloaded));
				created_before = driver()->calls().bind_groups_created;
				live_before = driver()->live_bind_groups();
			}
			if (frame == 9) {
				auto* texture = textures().find(reloaded);
				new_texture = texture ? texture->native() : nullptr;
				const auto& log = driver()->bind_log();
				for (std::size_t i = log_begin; i < log.size(); ++i) {
					const auto* info = log[i].set == material_set ? driver()->group_info(log[i].group) : nullptr;
					groups_with_the_texture += info && new_texture && info->entries[1].texture == new_texture ? 1 : 0;
				}
				created_after = driver()->calls().bind_groups_created;
				live_after = driver()->live_bind_groups();
			}
			log_begin = driver()->bind_log().size();
		});

	BOOST_REQUIRE_MESSAGE(new_texture, "the texture was not loaded again");
	// The materials were built again (the texture manager's generation moved) and so were their groups, the last
	// ones with the texture that was loaded after the removal.
	BOOST_TEST(groups_with_the_texture > 0u);
	BOOST_TEST(created_after > created_before);
	// The groups of the old materials are gone with them.
	BOOST_TEST(live_after == live_before);
	// No group of a destroyed texture was ever bound, no object used after its destruction.
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(TheCompilerPacksTheMaterialIntoThePodBlockOfTheShader)
{
	auto& render = storage.require<rnd::render_system>();
	rnd::material_compiler_interface* compiler = render.get_material_compiler();
	BOOST_REQUIRE(compiler);

	scn::material_desc desc;
	desc.albedo = ds::color{ 0.25f, 0.5f, 0.75f, 1.f };
	desc.emissive = ds::color{ 0.1f, 0.2f, 0.3f, 0.f };
	desc.shininess = 77.f;
	desc.vertex_shader = res::tag::make("shaders/scene.vert");
	desc.fragment_shader = res::tag::make("shaders/scene.frag");
	desc.samplers_textures = { res::tag{}, res::tag{} };	// two slots without a texture

	const auto config = compiler->compile(desc, rnd::material_compile_context{ &render.get_texture_manager(), false });

	rnd::material_params expected;
	expected.diffuse_color = desc.albedo;
	expected.emissive_color = desc.emissive;
	expected.shininess = 77.f;
	BOOST_REQUIRE(config.rdata.material_params.size() == sizeof(expected));
	BOOST_TEST(std::memcmp(config.rdata.material_params.data(), &expected, sizeof(expected)) == 0);
	BOOST_REQUIRE(config.rdata.samplers.size() == 2u);
	BOOST_TEST(config.rdata.samplers[0] == nullptr);

	// The block is bytes: nothing is left that the driver would set by name.
	static_assert(std::is_same_v<decltype(config.rdata.material_params), std::vector<std::byte>>);
}

BOOST_AUTO_TEST_SUITE_END()
