#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_material_groups.h>
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/rnd_shader_desc.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/level/scn_world.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/scene/scn_model.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
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
	void load_warm_level(std::string_view path)
	{
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make(path)));
		test_util::pump(tasks);
		// The meshes draw from the first frame, some with placeholder textures while theirs still load: the groups of
		// those frames go once unused for grace_frames, so warm means past that (#1021).
		constexpr int warm_frames = static_cast<int>(rnd::material_groups::grace_frames) + 2;
		BOOST_REQUIRE(run_frames(warm_frames) == warm_frames);
	}

	void load_warm_smoke_level() { load_warm_level("levels/smoke_level.desc"); }

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

	// The material instances the frame's draws point at.
	std::set<const rnd::compiled_material*> drawn_materials()
	{
		const auto& instances = storage.require<rnd::render_system>().material_instances();
		std::set<const rnd::compiled_material*> result;
		for (const auto& packet : packets()) {
			for (const auto& dc : packet.opaque_draws) result.insert(&instances.get(dc.material));
			for (const auto& dc : packet.transparent_draws) result.insert(&instances.get(dc.material));
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

	// What a material's group holds, as text: the params bytes and the three maps (null stands for a placeholder).
	std::string content_of(const rnd::compiled_material& material)
	{
		std::string key(reinterpret_cast<const char*>(material.runtime.material_params.data()), material.runtime.material_params.size());
		for (std::size_t slot = 0; slot < 3; ++slot) {
			const rnd::texture_id id = slot < material.runtime.samplers.size() ? material.runtime.samplers[slot] : rnd::texture_id{};
			const rnd::texture_binding bound = textures().binding(id);
			// The slot and its generation, as the group is keyed; a slot with no texture reads as a placeholder.
			key += '|' + std::to_string(bound.handle ? (std::uint64_t{ id.index } << 32) | bound.generation : 0);
		}
		return key;
	}

	rnd::texture_manager& textures() { return storage.require<rnd::render_system>().get_texture_manager(); }
};

}

BOOST_FIXTURE_TEST_SUITE(AppEngineNullBackendMaterialSet, material_set_fixture)

// Quads alone: any material shows its group bound, and no model import is needed (#977).
BOOST_AUTO_TEST_CASE(EveryDrawOfAMaterialHasItsGroupBound)
{
	load_warm_level("levels/smoke_quads.desc");
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

BOOST_AUTO_TEST_CASE(TheGroupHoldsWhatTheCompilerProducedForTheMaterial, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
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
		// One group per distinct contents, whatever the number of materials, draws, passes and cameras that use it.
		std::set<std::string> contents;
		for (const auto* material : materials) contents.insert(content_of(*material));
		BOOST_TEST(groups.size() == contents.size());

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
			if (material->runtime.samplers.empty() || !textures().is_ready(material->runtime.samplers[0])) {
				continue;
			}
			++textured;
			// The group is recorded by the backend under the handle the backend gave the texture, which the
			// validation layer's handle (what binding() returns) is not.
			const auto& tag = static_cast<const scn::material_desc*>(material->source.get())->samplers_textures[0];
			const rnd::texture_id texture_slot = textures().id_of(tag);
			BOOST_REQUIRE(textures().is_ready(texture_slot));
			const rnd::driver::texture_handle diffuse = validating_driver()->inner_handle(textures().binding(texture_slot).handle);
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

BOOST_AUTO_TEST_CASE(NoGroupIsRebuiltOnceTheSceneIsWarm, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
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

BOOST_AUTO_TEST_CASE(AMaterialRebuiltWithTheSameContentsKeepsItsGroupAndANewShininessGetsOne, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
{
	load_warm_smoke_level();
	driver()->record_bindings(true);

	entt::registry* registry = nullptr;
	entt::entity entity = entt::null;
	const void* desc_of_entity = nullptr;
	bool skinned_instance = false;
	std::uint64_t serial_before = 0;
	std::vector<std::uint64_t> serials;	// the dirtied instance's serial after each frame, from frame 1 on
	bool content_shared = false;
	std::vector<std::set<std::uint64_t>> bound;
	std::vector<std::size_t> created;
	std::vector<std::size_t> buffers;
	std::vector<std::size_t> live;
	std::size_t log_begin = driver()->bind_log().size();

	run_with(6,
		[&](int frame) {
			if (frame == 2) {
				// A real change: the compiled material is rebuilt and its contents are new.
				BOOST_REQUIRE(registry);
				auto& component = registry->get<scn::material_desc_component>(entity);
				component.mlt_desc->shininess += 10.f;
				component.dirty = true;
			}
			if (frame == 4) {
				// Only marked dirty: rebuilt with the contents it had.
				registry->get<scn::material_desc_component>(entity).dirty = true;
			}
		},
		[&](int frame) {
			if (frame == 1) {
				// A material that is drawn (the sky's is not): the one to dirty.
				const auto drawn = drawn_materials();
				storage.require<scn::context_manager>().level_set().for_each([&](scn::world_context& w) {
					if (registry) return;
					for (auto [ent, mat] : w.state().view<scn::material_desc_component>().each()) {
						const auto desc = mat.mlt_desc.get();
						const auto instance = std::ranges::find_if(drawn, [&](const rnd::compiled_material* m) { return m->source.get() == desc.get(); });
						if (desc && instance != drawn.end()) {
							registry = &w.state();
							entity = ent;
							const rnd::compiled_material* self = *instance;
							desc_of_entity = desc.get();
							skinned_instance = self->skinned;
							serial_before = self->serial;
							content_shared = std::ranges::any_of(drawn, [&](const rnd::compiled_material* other) {
								return other != self && content_of(*other) == content_of(*self);
							});
							return;
						}
					}
				});
				BOOST_REQUIRE(registry);
			}
			for (const auto* material : drawn_materials()) {
				if (material->source.get() == desc_of_entity && material->skinned == skinned_instance) {
					serials.push_back(material->serial);
				}
			}
			bound.push_back(material_groups_bound(log_begin));
			log_begin = driver()->bind_log().size();
			created.push_back(driver()->calls().bind_groups_created);
			buffers.push_back(driver()->calls().buffers_created);
			live.push_back(driver()->live_bind_groups());
		});

	BOOST_REQUIRE(bound.size() == 6u);
	// Rebuilt: a new compiled_material (its serial is never reused, its address may be).
	BOOST_REQUIRE(serials.size() == 5u);
	BOOST_TEST(serials.back() != serial_before);
	BOOST_TEST(serials.front() == serial_before);
	// Frame 2 built the changed material again: one group and one params buffer, nothing else.
	BOOST_TEST(created[2] == created[1] + 1);
	BOOST_TEST(buffers[2] == buffers[1] + 1);
	// The old contents are still bound when other entities have them, otherwise the new group replaces them.
	BOOST_TEST(bound[2].size() == bound[1].size() + (content_shared ? 1u : 0u));
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
	// Frame 4 rebuilt the material with the same contents: nothing was allocated.
	BOOST_TEST(created[4] == created[3]);
	BOOST_TEST(buffers[4] == buffers[3]);
	// The group of the old contents stays for its grace period at most (others may still use it).
	// The new group, and the old one (used by others, or kept for its grace period).
	BOOST_TEST(live[5] == live[1] + 1);
	expect_no_violations();
}

BOOST_AUTO_TEST_CASE(ReloadingATextureBuildsTheGroupsAgainWithTheNewTexture, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())
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
	rnd::driver::texture_handle new_texture;

	run_with(20,
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
				BOOST_REQUIRE(textures().is_ready(textures().id_of(reloaded)));
				created_before = driver()->calls().bind_groups_created;
				live_before = driver()->live_bind_groups();
			}
			if (frame == 19) {
				const rnd::driver::texture_handle loaded = textures().binding(textures().id_of(reloaded)).handle;
				new_texture = validating_driver()->inner_handle(loaded);
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

	BOOST_REQUIRE_MESSAGE(static_cast<bool>(new_texture), "the texture was not loaded again");
	// The materials were built again (the texture manager's generation moved) and so were their groups, the last
	// ones with the texture that was loaded after the removal.
	BOOST_TEST(groups_with_the_texture > 0u);
	BOOST_TEST(created_after > created_before);
	// The groups made while the texture was loading are gone after their grace period.
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
	desc.alpha_cutoff = 0.5f;
	desc.shader = storage.require<res::resource_system>().require_sync<rnd::shader_desc>(res::tag::make("shaders/scene.shader.desc"));
	desc.samplers_textures = { res::tag{}, res::tag{} };	// two slots without a texture

	const auto config = compiler->compile(desc, rnd::material_compile_context{ &render.get_texture_manager(), false, std::nullopt });

	rnd::material_params expected;
	expected.diffuse_color = desc.albedo;
	expected.emissive_color = desc.emissive;
	expected.shininess = 77.f;
	expected.alpha_cutoff = 0.5f;
	BOOST_REQUIRE(config.rdata.material_params.size() == sizeof(expected));
	BOOST_TEST(std::memcmp(config.rdata.material_params.data(), &expected, sizeof(expected)) == 0);
	BOOST_REQUIRE(config.rdata.samplers.size() == 2u);
	BOOST_TEST(!config.rdata.samplers[0].is_valid());

	// The block is bytes: nothing is left that the driver would set by name.
	static_assert(std::is_same_v<decltype(config.rdata.material_params), std::vector<std::byte>>);
}

// #1386: Check builds the program of a shader desc without a material, and leaves the renderer's problems and generation as they were.
BOOST_AUTO_TEST_CASE(ACheckOfAShaderDescBuildsItWithoutChangingTheRenderersProblems)
{
	auto& render = storage.require<rnd::render_system>();
	const std::uint64_t generation = render.shader_problems_generation();
	const std::size_t problems = render.shader_problems().size();
	std::vector<res::tag> files;
	const std::vector<rnd::shader_problem> found = render.check_shader_program(res::tag::make("shaders/scene.shader.desc"), &files);
	BOOST_TEST(found.empty());
	BOOST_TEST(!files.empty());
	BOOST_TEST(render.shader_problems_generation() == generation);
	BOOST_TEST(render.shader_problems().size() == problems);

	const std::vector<rnd::shader_problem> missing = render.check_shader_program(res::tag::make("shaders/no_such.shader.desc"));
	BOOST_REQUIRE(missing.size() == 1u);
	BOOST_TEST(missing[0].code == "shader/load");
	BOOST_TEST(render.shader_problems_generation() == generation);
}

// #1199, #1200: the material compiler builds the program and defines of the shader desc the material refers to.
BOOST_AUTO_TEST_CASE(AShaderDescGivesTheProgramTheCompilerBuildsFromTheSameStages)
{
	auto& render = storage.require<rnd::render_system>();
	rnd::material_compiler_interface* compiler = render.get_material_compiler();
	BOOST_REQUIRE(compiler);
	auto handle = storage.require<res::resource_system>().require_sync<rnd::shader_desc>(res::tag::make("shaders/scene.shader.desc"));
	test_util::pump(tasks);
	const auto shader = handle.get_sync();
	BOOST_REQUIRE(shader);

	scn::material_desc material;
	material.shader = handle;
	material.defines = { "USE_NORMAL_MAP" };

	for (const bool skinned : { false, true }) {
		BOOST_TEST_CONTEXT("skinned " << skinned) {
			const rnd::shader_config compiled = compiler->compile(material, rnd::material_compile_context{ &render.get_texture_manager(), skinned, std::nullopt });
			const rnd::shader_config config = shader->config(material.defines);
			BOOST_CHECK(config.cdata.program == compiled.cdata.program);
			// The skinning define stays the compiler's.
			std::vector<std::string> expected = config.cdata.defines;
			if (skinned) expected.push_back("USE_ANIMATION");
			BOOST_CHECK(expected == compiled.cdata.defines);
			BOOST_CHECK(config.rdata.samplers.empty());
			BOOST_CHECK(config.rdata.material_params.empty());
		}
	}
}

// #1439: the alpha test is added by build_compiled_material, whatever the compiler, only for a cutoff above 0, only once, and
// not to a shader that splits by alpha itself (the mix shader).
BOOST_AUTO_TEST_CASE(TheAlphaTestIsCompiledInOnlyForACutoffAboveZero)
{
	auto& render = storage.require<rnd::render_system>();
	rnd::material_compiler_interface* compiler = render.get_material_compiler();
	BOOST_REQUIRE(compiler);
	auto& resources = storage.require<res::resource_system>();
	const auto scene = resources.require_sync<rnd::shader_desc>(res::tag::make("shaders/scene.shader.desc"));
	const auto mix = resources.require_sync<rnd::shader_desc>(res::tag::make("shaders/mix_opaque_trans_scene.shader.desc"));
	test_util::pump(tasks);
	rnd::shader_config_cache variants;
	const auto defines_of = [&](const scn::material_desc& material) {
		const auto compiled = rnd::build_compiled_material(*compiler, variants, render.get_texture_manager(),
			std::make_shared<const scn::material_desc>(material), false);
		return variants.resolve(compiled->opaque).defines;
	};
	const auto count = [](const std::vector<std::string>& defines, std::string_view name) {
		return std::ranges::count(defines, std::string(name));
	};

	scn::material_desc material;
	material.shader = scene;
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_test_define) == 1);	// the default, 1: the old rule
	material.alpha_cutoff = 0.5f;
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_test_define) == 1);
	material.alpha_cutoff = 0.0f;
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_test_define) == 0);
	material.alpha_cutoff = 0.5f;
	material.defines = { std::string(rnd::k_alpha_test_define) };
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_test_define) == 1);

	// The compiler itself leaves the define to build_compiled_material, so a module's own compiler cannot lose it.
	material.defines.clear();
	const rnd::shader_config compiled = compiler->compile(material, rnd::material_compile_context{ &render.get_texture_manager(), false, std::nullopt });
	BOOST_TEST(count(compiled.cdata.defines, rnd::k_alpha_test_define) == 0);
	rnd::material_params packed;
	BOOST_REQUIRE(compiled.rdata.material_params.size() == sizeof(packed));
	std::memcpy(&packed, compiled.rdata.material_params.data(), sizeof(packed));
	BOOST_TEST(packed.alpha_cutoff == 0.5f);

	// The mix shader splits by alpha itself: no test, whatever the cutoff, and no variant of its own.
	material.shader = mix;
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_split_define) == 1);
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_test_define) == 0);
	material.alpha_cutoff = 0.0f;
	BOOST_TEST(count(defines_of(material), rnd::k_alpha_test_define) == 0);
}

namespace {

// The lines of `code` the preprocessor keeps with `defines` set: #ifdef / #ifndef / #if defined(X) / #elif defined(X) /
// #else / #endif, nested, every other line kept as it is (enough for the opaque shaders).
std::string active_code(const std::string& code, const std::vector<std::string>& defines)
{
	const auto is_defined = [&](std::string_view name) {
		return std::ranges::any_of(defines, [&](const std::string& d) { return d == name || d.starts_with(std::string(name) + " "); });
	};
	const auto name_in = [](std::string_view line, std::string_view after) {
		std::string_view rest = line.substr(line.find(after) + after.size());
		const std::size_t end = rest.find_first_of(") \t");
		return std::string(rest.substr(0, end));
	};
	struct level { bool parent; bool taken; bool active; };
	std::vector<level> stack;
	std::string out;
	std::size_t start = 0;
	while (start <= code.size()) {
		const std::size_t end = std::min(code.find('\n', start), code.size());
		const std::string_view line(code.data() + start, end - start);
		const std::string_view text = line.substr(std::min(line.find_first_not_of(" \t"), line.size()));
		const bool on = stack.empty() || stack.back().active;
		if (text.starts_with("#ifdef ") || text.starts_with("#ifndef ") || text.starts_with("#if defined(")) {
			const bool negated = text.starts_with("#ifndef ");
			const std::string name = text.starts_with("#if defined(") ? name_in(text, "defined(") : name_in(text, negated ? "#ifndef " : "#ifdef ");
			const bool value = is_defined(name) != negated;
			stack.push_back({ on, value, on && value });
		} else if (text.starts_with("#elif defined(") && !stack.empty()) {
			level& top = stack.back();
			const bool value = !top.taken && is_defined(name_in(text, "defined("));
			top.taken = top.taken || value;
			top.active = top.parent && value;
		} else if (text.starts_with("#else") && !stack.empty()) {
			level& top = stack.back();
			top.active = top.parent && !top.taken;
			top.taken = true;
		} else if (text.starts_with("#endif") && !stack.empty()) {
			stack.pop_back();
		} else if (on) {
			out.append(line).push_back(' ');
		}
		start = end + 1;
	}
	return out;
}

// What an `if (result.a < X) { ... discard; ... }` of the active code compares against; empty when no such test discards.
std::string discard_threshold(const std::string& active)
{
	const std::string open = "if (result.a < ";
	for (std::size_t at = active.find(open); at != std::string::npos; at = active.find(open, at + 1)) {
		const std::size_t value = at + open.size();
		const std::size_t close = active.find(')', value);
		const std::size_t brace_end = active.find('}', close);
		if (active.substr(close, brace_end - close).find("discard") != std::string::npos) {
			return active.substr(value, close - value);
		}
	}
	return {};
}

std::string read_shader_file(const char* name)
{
	std::ifstream in(std::filesystem::path{ SNK_CORE_RES_DIR } / "shaders" / name, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}

// #1439 review: the depth prepass draws every opaque material, so for each shader it must discard by the same rule as the
// shader's own opaque pass, with the defines that material really compiles with: a cutoff of 0.5 on scene, any on mix.
BOOST_AUTO_TEST_CASE(ThePrepassDiscardsByTheSameRuleAsTheOpaquePass_ForEveryOpaqueShader)
{
	auto& render = storage.require<rnd::render_system>();
	rnd::material_compiler_interface* compiler = render.get_material_compiler();
	BOOST_REQUIRE(compiler);
	auto& resources = storage.require<res::resource_system>();
	const std::string prepass = read_shader_file("z_prepass.frag");
	BOOST_REQUIRE(!prepass.empty());

	const std::pair<const char*, const char*> shaders[] = {
		{ "shaders/scene.shader.desc", "scene.frag" },
		{ "shaders/mix_opaque_trans_scene.shader.desc", "mix_opaque_trans_scene.frag" },
	};
	for (const auto& [desc, frag] : shaders) {
		for (const float cutoff : { 0.0f, 0.5f, 1.0f }) {
			BOOST_TEST_CONTEXT(desc << ", cutoff " << cutoff) {
				const auto shader = resources.require_sync<rnd::shader_desc>(res::tag::make(desc));
				test_util::pump(tasks);
				scn::material_desc material;
				material.shader = shader;
				material.alpha_cutoff = cutoff;
				rnd::shader_config_cache variants;
				const auto compiled = rnd::build_compiled_material(*compiler, variants, render.get_texture_manager(),
					std::make_shared<const scn::material_desc>(material), false);
				const std::vector<std::string> defines = variants.resolve(compiled->opaque).defines;
				const std::string colour = read_shader_file(frag);
				BOOST_REQUIRE(!colour.empty());
				BOOST_TEST(discard_threshold(active_code(prepass, defines)) == discard_threshold(active_code(colour, defines)));
			}
		}
	}
}

BOOST_AUTO_TEST_SUITE_END()
