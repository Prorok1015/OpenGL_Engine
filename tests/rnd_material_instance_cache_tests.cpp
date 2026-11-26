#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/scn_material_desc.h>
#include <snk/common/logger/engine_log_sinks.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// The material instance cache on the null backend (RHI R6b, #125): an instance is compiled once, again only when
// its description changed, a texture it uses was reloaded, or a map was still loading, and is dropped after
// grace_frames frames without a user.

namespace {

using namespace rnd::driver;

struct instance_cache_fixture : test_util::null_engine_fixture
{
	rnd::texture_manager& textures() { return storage.require<rnd::render_system>().get_texture_manager(); }
	rnd::material_compiler_interface* compiler() { return storage.require<rnd::render_system>().get_material_compiler(); }
	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	std::shared_ptr<scn::material_desc> make_desc(float shininess = 32.f, std::vector<res::tag> samplers = {})
	{
		auto desc = std::make_shared<scn::material_desc>();
		desc->shininess = shininess;
		desc->shader = resources().require_sync<rnd::shader_desc>(res::tag::make("shaders/scene.shader.desc"));
		desc->samplers_textures = std::move(samplers);
		return desc;
	}

	static bool has_no_stage(const rnd::shader_config::constant_data& cdata)
	{
		return std::ranges::none_of(cdata.program, [](const res::tag& stage) { return stage.is_valid(); });
	}

	// A new frame of the cache under test.
	void next_frame(rnd::material_instance_cache& cache) { cache.begin_frame(compiler()); }

	static bool has_define(const rnd::shader_config::constant_data& cdata, std::string_view define)
	{
		return std::ranges::find(cdata.defines, define) != cdata.defines.end();
	}

	// A test shader desc over the shipped scene stages, so no shipped file is rewritten.
	void write_shader(const res::tag& tag, std::string_view defines, std::string_view fragment = "res://shaders/scene.frag")
	{
		write_file(tag, std::string(R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/scene.vert","fragment":")")
			+ std::string(fragment) + R"(","defines":)" + std::string(defines) + "}");
	}

	void write_file(const res::tag& tag, const std::string& body)
	{
		BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(body)));
	}

	// What a hot reload does: the file changed, the watchers are told on the main thread.
	void signal(const res::tag& tag)
	{
		resources().signal_changed(tag);
		test_util::pump(tasks);
	}

	std::shared_ptr<scn::material_desc> make_desc_with_shader(const res::tag& shader, std::vector<res::tag> samplers = {})
	{
		auto desc = make_desc(32.f, std::move(samplers));
		desc->shader = resources().require<rnd::shader_desc>(shader);
		test_util::pump(tasks);
		return desc;
	}
};

// Counts the errors logged with `part` in their text.
struct error_count
{
	explicit error_count(std::string part)
		: m_part(std::move(part))
	{
		sink = engine::add_log_sink([this](std::string_view, engine::log_level level, std::string_view message) {
			if (level == engine::log_level::error && message.find(m_part) != std::string_view::npos) ++count;
		});
	}

	std::string m_part;
	std::atomic<int> count{ 0 };
	engine::log_sink_handle sink;	// last: removed first
};

}

BOOST_FIXTURE_TEST_SUITE(RndMaterialInstanceCacheTests, instance_cache_fixture)

BOOST_AUTO_TEST_CASE(AnInstanceIsCompiledOnceAndReusedByEveryAskAndEveryFrame)
{
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	const auto desc = make_desc();

	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	BOOST_REQUIRE(id.is_valid());
	BOOST_REQUIRE(cache.contains(id));
	BOOST_TEST(cache.compilations() == 1u);
	const std::uint64_t serial = cache.get(id).serial;
	BOOST_TEST(cache.get(id).base.is_valid());
	BOOST_TEST(cache.get(id).opaque.is_valid());
	BOOST_TEST(cache.get(id).complete);

	// Another mesh and another camera with the same material, in the same frame and in later ones.
	for (int frame = 0; frame < 5; ++frame) {
		next_frame(cache);
		for (int ask = 0; ask < 4; ++ask) {
			BOOST_CHECK(cache.acquire(desc, false, false) == id);
		}
	}
	BOOST_TEST(cache.compilations() == 1u);
	BOOST_TEST(cache.size() == 1u);
	BOOST_TEST(cache.get(id).serial == serial);
}

BOOST_AUTO_TEST_CASE(StaticAndSkinnedMeshesAndDifferentDescsHaveTheirOwnInstances)
{
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	const auto first = make_desc();
	const auto second = make_desc();

	next_frame(cache);
	const auto plain = cache.acquire(first, false, false);
	const auto skinned = cache.acquire(first, true, false);
	const auto other = cache.acquire(second, false, false);
	BOOST_CHECK(plain != skinned);
	BOOST_CHECK(plain != other);
	BOOST_CHECK(skinned != other);
	BOOST_TEST(cache.size() == 3u);
	BOOST_TEST(!cache.get(plain).skinned);
	BOOST_TEST(cache.get(skinned).skinned);
	// The skinned variant defines USE_ANIMATION, so it draws with another program.
	BOOST_CHECK(cache.get(plain).base != cache.get(skinned).base);
	// Equal descs share the shader variant, not the instance.
	BOOST_CHECK(cache.get(plain).base == cache.get(other).base);
}

BOOST_AUTO_TEST_CASE(ADescChangeRebuildsTheInstanceInPlaceOncePerFrame)
{
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	const auto desc = make_desc(10.f);

	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	const auto skinned_id = cache.acquire(desc, true, false);
	const std::uint64_t serial = cache.get(id).serial;
	const std::uint64_t skinned_serial = cache.get(skinned_id).serial;
	const auto params_before = cache.get(id).runtime.material_params;
	BOOST_TEST(cache.compilations() == 2u);

	next_frame(cache);
	desc->shininess = 99.f;
	// Several meshes share the desc and are all marked.
	for (int mesh = 0; mesh < 3; ++mesh) {
		BOOST_CHECK(cache.acquire(desc, false, true) == id);	// the handle stays
	}
	// Once per instance of the desc, not once per mesh; the skinned instance follows the edit whichever mesh marked it.
	BOOST_TEST(cache.compilations() == 4u);
	BOOST_TEST(cache.get(id).serial != serial);
	BOOST_TEST((cache.get(id).runtime.material_params != params_before));
	BOOST_TEST(cache.get(skinned_id).serial != skinned_serial);

	// A desc replaced by another object is another key: the new one is compiled, the old one waits for eviction.
	next_frame(cache);
	const auto replacement = make_desc(10.f);
	const auto replaced = cache.acquire(replacement, false, false);
	BOOST_CHECK(replaced != id);
	BOOST_TEST(cache.compilations() == 5u);
}

BOOST_AUTO_TEST_CASE(ATextureReloadOrRenderTargetResizeRebuildsTheInstance)
{
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	const auto tag = res::tag(res::tag::memory, "__material_instance_cache_map");
	const auto usage = texture_usage::color_target | texture_usage::sampled;
	const auto first_map = rnd::render_targets::ensure(textures(), tag, { 8, 8 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(first_map);
	const auto desc = make_desc(32.f, { tag });

	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	BOOST_REQUIRE(cache.get(id).complete);
	BOOST_REQUIRE(cache.get(id).runtime.samplers.size() == 1u);
	const rnd::texture_id slot = textures().id_of(tag);
	BOOST_REQUIRE(slot.is_valid());
	BOOST_TEST((cache.get(id).runtime.samplers[0] == slot));	// the material holds the id, not the texture
	const auto generation_before = textures().binding(slot).generation;
	const std::uint64_t serial = cache.get(id).serial;

	// Nothing changed: nothing compiled.
	next_frame(cache);
	BOOST_CHECK(cache.acquire(desc, false, false) == id);
	BOOST_TEST(cache.compilations() == 1u);

	// What a hot reload of the picture does to the texture manager: the texture is replaced, the generation moves.
	const auto second_map = rnd::render_targets::ensure(textures(), tag, { 16, 16 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(second_map);
	next_frame(cache);
	BOOST_CHECK(cache.acquire(desc, false, false) == id);
	BOOST_TEST(cache.compilations() == 2u);
	BOOST_TEST(cache.get(id).serial != serial);
	BOOST_TEST((cache.get(id).runtime.samplers[0] == slot));	// the slot survived the resize...
	BOOST_TEST((textures().id_of(tag) == slot));
	BOOST_TEST(textures().binding(slot).generation != generation_before);	// ...and its generation moved

	// And the frame after it is quiet again.
	next_frame(cache);
	cache.acquire(desc, false, false);
	BOOST_TEST(cache.compilations() == 2u);
}

BOOST_AUTO_TEST_CASE(AMapThatIsStillLoadingIsAskedForAgainOncePerFrame)
{
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	// A picture that never loads (there is no such file).
	const auto desc = make_desc(32.f, { res::tag::make("textures/__material_instance_cache_missing__.desc") });

	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	BOOST_TEST(!cache.get(id).complete);
	cache.acquire(desc, false, false);
	cache.acquire(desc, false, false);
	BOOST_TEST(cache.compilations() == 1u);	// the asks of one frame share one attempt

	for (std::uint64_t frame = 2; frame <= 4; ++frame) {
		next_frame(cache);
		for (int ask = 0; ask < 3; ++ask) {
			BOOST_CHECK(cache.acquire(desc, false, false) == id);
		}
		BOOST_TEST(cache.compilations() == frame);	// one attempt per frame
	}
}

BOOST_AUTO_TEST_CASE(AnInstanceNobodyAsksForIsDroppedAfterTheGracePeriodAndItsSlotIsReused)
{
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	const auto kept = make_desc();
	auto unused = make_desc();
	const std::weak_ptr<scn::material_desc> unused_watch = unused;

	next_frame(cache);
	const auto kept_id = cache.acquire(kept, false, false);
	const auto unused_id = cache.acquire(unused, false, false);
	unused.reset();	// the scene let the desc go; the instance keeps it alive
	BOOST_TEST(cache.size() == 2u);

	for (std::uint64_t frame = 0; frame < rnd::material_instance_cache::grace_frames; ++frame) {
		next_frame(cache);
		cache.acquire(kept, false, false);
		BOOST_CHECK(cache.contains(unused_id));
	}
	BOOST_TEST(!unused_watch.expired());

	next_frame(cache);
	BOOST_CHECK(cache.acquire(kept, false, false) == kept_id);
	BOOST_TEST(!cache.contains(unused_id));
	BOOST_TEST(cache.size() == 1u);
	BOOST_TEST(unused_watch.expired());	// the description is released with the instance

	// A material that was dropped is compiled again when it comes back, in a free slot.
	const auto comeback = make_desc();
	const auto compilations = cache.compilations();
	const auto comeback_id = cache.acquire(comeback, false, false);
	BOOST_TEST(cache.compilations() == compilations + 1);
	BOOST_CHECK(comeback_id == unused_id);
	BOOST_TEST(cache.get(comeback_id).source.get() == comeback.get());
}

// #1200: the program comes from the shader desc the material refers to. While it loads the instance is built again each
// frame, like a map still loading; once it has loaded it is the shader's program.
BOOST_AUTO_TEST_CASE(AShaderStillLoadingLeavesTheInstanceIncompleteUntilItLoads)
{
	const res::tag tag{ res::tag::memory, "t/pending.shader.desc" };
	BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(R"({"__type":"shader_desc","__version":1,
		"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag","defines":[]})")));
	const auto desc = make_desc();
	desc->shader = resources().require<rnd::shader_desc>(tag);
	BOOST_REQUIRE(!desc->shader.is_ready());
	BOOST_REQUIRE(!desc->shader.has_error());

	rnd::shader_config_cache variants;
	const auto pending = rnd::build_compiled_material(*compiler(), variants, textures(), desc, false);
	BOOST_TEST(!pending->complete);
	BOOST_TEST(has_no_stage(variants.resolve(pending->base)));

	test_util::pump(tasks);
	BOOST_REQUIRE(desc->shader.is_ready());
	const auto loaded = rnd::build_compiled_material(*compiler(), variants, textures(), desc, false);
	BOOST_TEST(loaded->complete);
	BOOST_TEST(variants.resolve(loaded->base).program.get_vertex_shader().string() == "res://shaders/scene.vert");
	BOOST_TEST(variants.resolve(loaded->base).program.get_fragment_shader().string() == "res://shaders/scene.frag");
}

// build_compiled_material reads the shader once and hands that state to compile: a shader that finishes loading between
// two reads must not give an empty program marked complete.
BOOST_AUTO_TEST_CASE(CompileUsesTheShaderStateItIsHanded_NotASecondRead)
{
	const auto desc = make_desc();
	BOOST_REQUIRE(desc->shader.is_ready());

	rnd::material_compile_context still_loading{ &textures(), false, rnd::material_shader_state{} };
	BOOST_TEST(has_no_stage(compiler()->compile(*desc, still_loading).cdata));

	rnd::material_compile_context loaded{ &textures(), false, rnd::read_material_shader(*desc) };
	BOOST_TEST(compiler()->compile(*desc, loaded).cdata.program.get_vertex_shader().string() == "res://shaders/scene.vert");
}

// A shader that failed is final: the instance is complete with no program (not drawn) and is not compiled again.
BOOST_AUTO_TEST_CASE(AShaderThatFailedToLoadIsNotDrawnAndNotRetried)
{
	const auto desc = make_desc();
	desc->shader = resources().require<rnd::shader_desc>(res::tag::make("shaders/__material_instance_cache_missing__.shader.desc"));
	test_util::pump(tasks);
	BOOST_REQUIRE(desc->shader.has_error());

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures());
	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	BOOST_TEST(cache.get(id).complete);
	BOOST_TEST(has_no_stage(variants.resolve(cache.get(id).base)));
	for (int frame = 0; frame < 3; ++frame) {
		next_frame(cache);
		BOOST_CHECK(cache.acquire(desc, false, false) == id);
	}
	BOOST_TEST(cache.compilations() == 1u);
}

BOOST_AUTO_TEST_CASE(AMaterialWithoutAShaderIsNotDrawn)
{
	const auto desc = make_desc();
	desc->shader = {};

	rnd::shader_config_cache variants;
	const auto built = rnd::build_compiled_material(*compiler(), variants, textures(), desc, false);
	BOOST_TEST(built->complete);
	BOOST_TEST(has_no_stage(variants.resolve(built->base)));
}

// #1272: a .shader.desc that changed on disk reaches the instances of the materials that use it, though the desc still
// holds the old handle (no dependency edge from the material to its shader).
BOOST_AUTO_TEST_CASE(AChangedShaderDescIsPickedUpWithItsNewDefines)
{
	const res::tag tag{ res::tag::memory, "t/reload_defines.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);
	BOOST_REQUIRE(desc->shader.is_ready());

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	const std::uint64_t serial = cache.get(id).serial;
	BOOST_TEST(!has_define(variants.resolve(cache.get(id).base), "USE_NORMAL_MAP"));

	write_shader(tag, R"(["USE_NORMAL_MAP"])");
	signal(tag);
	next_frame(cache);
	cache.acquire(desc, false, false);	// asks for the changed shader
	test_util::pump(tasks);
	next_frame(cache);
	BOOST_CHECK(cache.acquire(desc, false, false) == id);
	BOOST_TEST(cache.get(id).serial != serial);
	BOOST_TEST(has_define(variants.resolve(cache.get(id).base), "USE_NORMAL_MAP"));
	BOOST_TEST(cache.compilations() == 2u);

	// A later rebuild for another reason keeps the reloaded shader, not the one the desc still holds.
	desc->shininess = 5.f;
	next_frame(cache);
	cache.acquire(desc, false, true);
	BOOST_TEST(cache.compilations() == 3u);
	BOOST_TEST(has_define(variants.resolve(cache.get(id).base), "USE_NORMAL_MAP"));
}

BOOST_AUTO_TEST_CASE(TheOldInstanceDrawsWhileTheChangedShaderLoads)
{
	const res::tag tag{ res::tag::memory, "t/reload_loading.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);
	const std::size_t watched = resources().watched_tag_count();
	{
		rnd::shader_config_cache variants;
		rnd::material_instance_cache cache(variants, textures(), &resources());
		next_frame(cache);
		const auto id = cache.acquire(desc, false, false);
		const std::uint64_t serial = cache.get(id).serial;
		BOOST_TEST(resources().watched_tag_count() == watched + 1);

		write_shader(tag, R"(["USE_NORMAL_MAP"])");
		signal(tag);
		// No task runs between these frames, so the new shader's load stays queued.
		for (int frame = 0; frame < 3; ++frame) {
			next_frame(cache);
			BOOST_CHECK(cache.acquire(desc, false, false) == id);
			BOOST_TEST(cache.get(id).serial == serial);
		}
		BOOST_TEST(cache.compilations() == 1u);

		test_util::pump(tasks);
		next_frame(cache);
		cache.acquire(desc, false, false);
		BOOST_TEST(cache.get(id).serial != serial);
		BOOST_TEST(cache.compilations() == 2u);
	}
	BOOST_TEST(resources().watched_tag_count() == watched);	// the cache let its watch go
}

BOOST_AUTO_TEST_CASE(AShaderThatFailsToReloadKeepsTheOldInstanceAndIsReportedOnce)
{
	const res::tag tag{ res::tag::memory, "t/reload_failed.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	const std::uint64_t serial = cache.get(id).serial;
	const std::uint64_t generation = cache.problems_generation();

	write_file(tag, "{ not json");
	signal(tag);
	for (int frame = 0; frame < 3; ++frame) {
		next_frame(cache);
		BOOST_CHECK(cache.acquire(desc, false, false) == id);
		test_util::pump(tasks);
	}
	BOOST_TEST(cache.get(id).serial == serial);
	BOOST_TEST(cache.compilations() == 1u);
	BOOST_TEST(cache.get(id).base.is_valid());
	BOOST_TEST(variants.resolve(cache.get(id).base).program.get_fragment_shader().string() == "res://shaders/scene.frag");
	const auto problems = cache.problems();
	BOOST_REQUIRE(problems.size() == 1u);
	BOOST_TEST(problems[0].file == tag);
	BOOST_TEST(problems[0].code == "shader/load");
	BOOST_TEST(!problems[0].message.empty());
	BOOST_TEST(cache.problems_generation() != generation);

	write_shader(tag, "[]", "res://shaders/mix_opaque_trans_scene.frag");
	signal(tag);
	next_frame(cache);
	cache.acquire(desc, false, false);
	test_util::pump(tasks);
	next_frame(cache);
	cache.acquire(desc, false, false);
	BOOST_TEST(cache.problems().empty());
	BOOST_TEST(cache.get(id).serial != serial);
	BOOST_TEST(variants.resolve(cache.get(id).base).program.get_fragment_shader().string() == "res://shaders/mix_opaque_trans_scene.frag");
}

// #1273: a material whose shader failed and whose map is still loading is rebuilt every frame; its shader is reported
// once, not on every rebuild.
BOOST_AUTO_TEST_CASE(AFailedShaderIsReportedOnceThoughTheInstanceIsRebuiltEveryFrame)
{
	const res::tag shader = res::tag::make("shaders/__material_instance_cache_missing__.shader.desc");
	const auto desc = make_desc_with_shader(shader, { res::tag::make("textures/__material_instance_cache_missing__.desc") });
	BOOST_REQUIRE(desc->shader.has_error());

	error_count errors(std::string(shader.view()));
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	for (std::uint64_t frame = 1; frame <= 4; ++frame) {
		next_frame(cache);
		cache.acquire(desc, false, false);
		BOOST_TEST(cache.compilations() == frame);
	}
	BOOST_TEST(cache.problems().size() == 1u);
	BOOST_TEST(cache.problems()[0].file == shader);
	BOOST_TEST(errors.count.load() == 1);
}

BOOST_AUTO_TEST_CASE(AnInstanceDroppedWhileItsShaderReloadsLetsItsWatchGo)
{
	const res::tag tag{ res::tag::memory, "t/evict_pending.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);
	const std::size_t watched = resources().watched_tag_count();

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	write_file(tag, "{ not json");
	signal(tag);
	next_frame(cache);
	cache.acquire(desc, false, false);	// the failing reload is asked for and stays queued
	for (std::uint64_t frame = 0; frame <= rnd::material_instance_cache::grace_frames; ++frame) {
		next_frame(cache);
	}
	BOOST_TEST(cache.size() == 0u);
	test_util::pump(tasks);
	next_frame(cache);
	BOOST_TEST(resources().watched_tag_count() == watched);
	BOOST_TEST(cache.problems().empty());
}

BOOST_AUTO_TEST_CASE(AShaderThatFailsAgainAfterAFixIsReportedAgain)
{
	const res::tag tag{ res::tag::memory, "t/fail_fix_fail.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	// Signal, then two frames: the first asks for the changed shader, the second sees how its load ended.
	auto reload = [&](const std::string& body) {
		write_file(tag, body);
		signal(tag);
		next_frame(cache);
		cache.acquire(desc, false, false);
		test_util::pump(tasks);
		next_frame(cache);
		cache.acquire(desc, false, false);
	};
	const std::string valid = R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/scene.vert","fragment":"res://shaders/scene.frag","defines":[]})";

	std::uint64_t generation = cache.problems_generation();
	reload("{ not json");
	BOOST_TEST(cache.problems().size() == 1u);
	BOOST_TEST(cache.problems_generation() > generation);
	generation = cache.problems_generation();

	reload(valid);
	BOOST_TEST(cache.problems().empty());
	BOOST_TEST(cache.problems_generation() > generation);
	generation = cache.problems_generation();

	reload("{ not json");
	BOOST_TEST(cache.problems().size() == 1u);
	BOOST_TEST(cache.problems_generation() > generation);
}

// A second failure with another reason updates the row the panel shows without logging the pair again.
BOOST_AUTO_TEST_CASE(ASecondFailureWithAnotherReasonUpdatesTheProblemWithoutLoggingAgain)
{
	const res::tag tag{ res::tag::memory, "t/fail_twice.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	error_count errors("the shader '" + std::string(tag.view()) + "'");
	auto reload = [&](const std::string& body) {
		write_file(tag, body);
		signal(tag);
		next_frame(cache);
		cache.acquire(desc, false, false);
		test_util::pump(tasks);
		next_frame(cache);
		cache.acquire(desc, false, false);
	};

	reload("{ not json");
	BOOST_REQUIRE(cache.problems().size() == 1u);
	const std::string first = cache.problems()[0].message;
	const std::uint64_t generation = cache.problems_generation();

	reload(R"({"__type":"__no_such_desc__","__version":1})");
	BOOST_REQUIRE(cache.problems().size() == 1u);
	BOOST_TEST(cache.problems()[0].message != first);
	BOOST_TEST(cache.problems_generation() > generation);
	BOOST_TEST(errors.count.load() == 1);
}

BOOST_AUTO_TEST_CASE(TheProblemOfAMaterialNobodyDrawsGoesWithItsInstance)
{
	const auto desc = make_desc_with_shader(res::tag::make("shaders/__material_instance_cache_missing__.shader.desc"));
	BOOST_REQUIRE(desc->shader.has_error());

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	cache.acquire(desc, true, false);	// the skinned sibling shares the entry
	BOOST_REQUIRE(cache.problems().size() == 1u);
	const std::uint64_t generation = cache.problems_generation();

	for (std::uint64_t frame = 0; frame <= rnd::material_instance_cache::grace_frames; ++frame) {
		next_frame(cache);
	}
	BOOST_TEST(cache.size() == 0u);
	BOOST_TEST(cache.problems().empty());
	BOOST_TEST(cache.problems_generation() > generation);
}

BOOST_AUTO_TEST_CASE(EachFailedShaderAndMaterialPairIsReportedOnce)
{
	const res::tag first = res::tag::make("shaders/__material_instance_cache_missing_a__.shader.desc");
	const res::tag second = res::tag::make("shaders/__material_instance_cache_missing_b__.shader.desc");
	const res::tag map = res::tag::make("textures/__material_instance_cache_missing__.desc");
	const auto a = make_desc_with_shader(first, { map });
	const auto b = make_desc_with_shader(second, { map });
	BOOST_REQUIRE(a->shader.has_error());
	BOOST_REQUIRE(b->shader.has_error());

	error_count errors("the material is not drawn");
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	for (int frame = 0; frame < 3; ++frame) {
		next_frame(cache);
		cache.acquire(a, false, false);
		cache.acquire(a, true, false);
		cache.acquire(b, false, false);
	}
	BOOST_TEST(cache.compilations() == 9u);	// rebuilt every frame: the map never loads
	const auto problems = cache.problems();
	BOOST_REQUIRE(problems.size() == 2u);
	BOOST_TEST(std::ranges::count_if(problems, [&](const rnd::shader_problem& p) { return p.file == first; }) == 1);
	BOOST_TEST(std::ranges::count_if(problems, [&](const rnd::shader_problem& p) { return p.file == second; }) == 1);
	BOOST_TEST(errors.count.load() == 2);
}

// The material is pointed at another shader: the failed one's problem is not the material's any more.
BOOST_AUTO_TEST_CASE(AMaterialRetargetedToAGoodShaderLosesTheOldShadersProblem)
{
	const res::tag good{ res::tag::memory, "t/retarget_good.shader.desc" };
	write_shader(good, "[]");
	const auto desc = make_desc_with_shader(res::tag::make("shaders/__material_instance_cache_missing__.shader.desc"));
	BOOST_REQUIRE(desc->shader.has_error());

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	cache.acquire(desc, true, false);
	BOOST_REQUIRE(cache.problems().size() == 1u);
	const std::uint64_t generation = cache.problems_generation();

	desc->shader = resources().require<rnd::shader_desc>(good);
	test_util::pump(tasks);
	BOOST_REQUIRE(desc->shader.is_ready());
	next_frame(cache);
	BOOST_CHECK(cache.acquire(desc, false, true) == id);	// an edit in place: the same instance
	BOOST_TEST(cache.problems().empty());
	BOOST_TEST(cache.problems_generation() > generation);
	BOOST_TEST(!has_no_stage(variants.resolve(cache.get(id).base)));
}

BOOST_AUTO_TEST_CASE(ACacheDestroyedWithAChangeStillQueuedLetsItsWatchGo)
{
	const res::tag tag{ res::tag::memory, "t/destroyed_queued.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);
	const std::size_t watched = resources().watched_tag_count();
	{
		rnd::shader_config_cache variants;
		rnd::material_instance_cache cache(variants, textures(), &resources());
		next_frame(cache);
		cache.acquire(desc, false, false);
		write_shader(tag, R"(["USE_NORMAL_MAP"])");
		resources().signal_changed(tag);	// the watchers are told on the next pump, after the cache is gone
	}
	test_util::pump(tasks);
	BOOST_TEST(resources().watched_tag_count() == watched);
}

// #1440: a material of the transparent or mix queue whose shader writes nothing the transparent pass can blend is drawn as
// before and reported once per shader and material, until the queue or the shader suits it.
BOOST_AUTO_TEST_CASE(ABlendedQueueWithAShaderWithoutATransparentPathIsReportedOnceUntilFixed)
{
	const res::tag opaque_only{ res::tag::memory, "t/opaque_only.shader.desc" };
	write_shader(opaque_only, "[]");
	const auto desc = make_desc_with_shader(opaque_only);
	BOOST_REQUIRE(desc->shader.is_ready());
	BOOST_TEST(!desc->shader.get()->transparent);

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	BOOST_TEST(cache.problems().empty());	// opaque: nothing to say

	for (const scn::pass_queue queue : { scn::pass_queue::TRANSPARENT, scn::pass_queue::MIX }) {
		BOOST_TEST_CONTEXT("queue " << static_cast<int>(queue)) {
			desc->queue = queue;
			next_frame(cache);
			const auto id = cache.acquire(desc, false, true);
			cache.acquire(desc, true, false);
			BOOST_TEST(id.is_valid());	// still drawn
			BOOST_REQUIRE(cache.problems().size() == 1u);
			const rnd::shader_problem problem = cache.problems().front();
			BOOST_TEST(problem.code == std::string(rnd::k_no_transparent_path_code));
			BOOST_TEST(problem.file == opaque_only);
			BOOST_TEST(problem.program == std::string(desc->get_tag().view()));
			BOOST_TEST(problem.message.find("mix_opaque_trans_scene.shader.desc") != std::string::npos);

			// Another frame and another compile of the same pair: still one row, the generation stays.
			const std::uint64_t generation = cache.problems_generation();
			next_frame(cache);
			cache.acquire(desc, false, true);
			BOOST_TEST(cache.problems().size() == 1u);
			BOOST_TEST(cache.problems_generation() == generation);
		}
	}

	desc->queue = scn::pass_queue::OPAQUE;
	next_frame(cache);
	cache.acquire(desc, false, true);
	cache.acquire(desc, true, true);
	BOOST_TEST(cache.problems().empty());
}

BOOST_AUTO_TEST_CASE(AShaderThatDeclaresATransparentPath_SuitsTheBlendedQueues)
{
	const res::tag blended{ res::tag::memory, "t/blended.shader.desc" };
	write_file(blended, R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/scene.vert",)"
		R"("fragment":"res://shaders/mix_opaque_trans_scene.frag","defines":[],"transparent":true})");
	const auto desc = make_desc_with_shader(blended);
	BOOST_REQUIRE(desc->shader.is_ready());
	BOOST_TEST(desc->shader.get()->transparent);

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	for (const scn::pass_queue queue : { scn::pass_queue::TRANSPARENT, scn::pass_queue::MIX }) {
		desc->queue = queue;
		next_frame(cache);
		cache.acquire(desc, false, true);
		BOOST_TEST(cache.problems().empty());
	}
}

// The shipped shaders say what they can draw: the mix shader has the transparent path, the scene shader has not.
BOOST_AUTO_TEST_CASE(TheShippedMixShaderHasATransparentPath_TheSceneShaderHasNot)
{
	const auto mix = resources().require_sync<rnd::shader_desc>(res::tag::make("shaders/mix_opaque_trans_scene.shader.desc"));
	const auto scene = resources().require_sync<rnd::shader_desc>(res::tag::make("shaders/scene.shader.desc"));
	test_util::pump(tasks);
	BOOST_REQUIRE(mix.get_sync());
	BOOST_REQUIRE(scene.get_sync());
	BOOST_TEST(mix.get_sync()->transparent);
	BOOST_TEST(!scene.get_sync()->transparent);
}

// A shader that changes so that it suits the queue takes the problem away on its reload.
BOOST_AUTO_TEST_CASE(AShaderReloadedWithATransparentPathLosesTheProblem)
{
	const res::tag tag{ res::tag::memory, "t/gains_transparent.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);
	desc->queue = scn::pass_queue::TRANSPARENT;

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	BOOST_REQUIRE(cache.problems().size() == 1u);

	write_file(tag, R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/scene.vert",)"
		R"("fragment":"res://shaders/mix_opaque_trans_scene.frag","defines":[],"transparent":true})");
	signal(tag);
	for (int frame = 0; frame < 4 && !cache.problems().empty(); ++frame) {
		next_frame(cache);
		cache.acquire(desc, false, false);
		test_util::pump(tasks);
	}
	BOOST_TEST(cache.problems().empty());
}

// The other way round: a reload that loses the path reports it.
BOOST_AUTO_TEST_CASE(AShaderReloadedWithoutItsTransparentPathIsReported)
{
	const res::tag tag{ res::tag::memory, "t/loses_transparent.shader.desc" };
	write_file(tag, R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/scene.vert",)"
		R"("fragment":"res://shaders/mix_opaque_trans_scene.frag","defines":[],"transparent":true})");
	const auto desc = make_desc_with_shader(tag);
	desc->queue = scn::pass_queue::MIX;

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	cache.acquire(desc, false, false);
	BOOST_REQUIRE(cache.problems().empty());

	write_shader(tag, "[]");
	signal(tag);
	for (int frame = 0; frame < 4 && cache.problems().empty(); ++frame) {
		next_frame(cache);
		cache.acquire(desc, false, false);
		test_util::pump(tasks);
	}
	BOOST_REQUIRE(cache.problems().size() == 1u);
	BOOST_TEST(cache.problems().front().code == std::string(rnd::k_no_transparent_path_code));
}

// A material nobody draws any more takes its problem with it; one pointed at a shader with the path loses it at once.
BOOST_AUTO_TEST_CASE(TheQueueProblemGoesWithTheInstance_AndWithTheOldShader)
{
	const res::tag opaque_only{ res::tag::memory, "t/queue_opaque_only.shader.desc" };
	const res::tag blended{ res::tag::memory, "t/queue_blended.shader.desc" };
	write_shader(opaque_only, "[]");
	write_file(blended, R"({"__type":"shader_desc","__version":1,"vertex":"res://shaders/scene.vert",)"
		R"("fragment":"res://shaders/mix_opaque_trans_scene.frag","defines":[],"transparent":true})");
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());

	{
		const auto dropped = make_desc_with_shader(opaque_only);
		dropped->queue = scn::pass_queue::TRANSPARENT;
		next_frame(cache);
		cache.acquire(dropped, false, false);
		BOOST_REQUIRE(cache.problems().size() == 1u);
		for (std::uint64_t frame = 0; frame <= rnd::material_instance_cache::grace_frames; ++frame) {
			next_frame(cache);
		}
		BOOST_TEST(cache.size() == 0u);
		BOOST_TEST(cache.problems().empty());
	}

	const auto retargeted = make_desc_with_shader(opaque_only);
	retargeted->queue = scn::pass_queue::TRANSPARENT;
	next_frame(cache);
	const auto id = cache.acquire(retargeted, false, false);
	BOOST_REQUIRE(cache.problems().size() == 1u);
	retargeted->shader = resources().require<rnd::shader_desc>(blended);
	test_util::pump(tasks);
	BOOST_REQUIRE(retargeted->shader.is_ready());
	next_frame(cache);
	BOOST_CHECK(cache.acquire(retargeted, false, true) == id);
	BOOST_TEST(cache.problems().empty());
}

// The render system's own cache: its problems reach shader_problems() and move its generation.
BOOST_AUTO_TEST_CASE(TheRenderSystemReportsTheMaterialCachesProblems)
{
	auto& render = storage.require<rnd::render_system>();
	rnd::material_instance_cache& cache = render.material_instances();
	const res::tag shader = res::tag::make("shaders/__material_instance_cache_render_missing__.shader.desc");
	const auto desc = make_desc_with_shader(shader);
	BOOST_REQUIRE(desc->shader.has_error());
	const std::uint64_t generation = render.shader_problems_generation();
	const std::size_t before = render.shader_problems().size();

	// No frame runs in this fixture, so the case drives the cache's frame itself.
	next_frame(cache);
	cache.acquire(desc, false, false);
	BOOST_TEST(render.shader_problems_generation() > generation);
	const auto problems = render.shader_problems();
	BOOST_REQUIRE(problems.size() == before + 1);
	BOOST_TEST(problems.back().file == shader);
	BOOST_TEST(problems.back().code == "shader/load");
}

// One sibling's reload failed: the other's good compile, with the old shader, must not take the shared problem away.
BOOST_AUTO_TEST_CASE(ASiblingsGoodCompileKeepsTheProblemOfAFailedReload)
{
	const res::tag tag{ res::tag::memory, "t/sibling_reload.shader.desc" };
	write_shader(tag, "[]");
	const auto desc = make_desc_with_shader(tag);

	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache(variants, textures(), &resources());
	next_frame(cache);
	const auto plain = cache.acquire(desc, false, false);
	cache.acquire(desc, true, false);
	const std::uint64_t compiled = cache.compilations();

	write_file(tag, "{ not json");
	signal(tag);
	// Only the skinned instance is asked for: its reload starts, then fails.
	next_frame(cache);
	cache.acquire(desc, true, false);
	test_util::pump(tasks);
	next_frame(cache);
	cache.acquire(desc, true, false);
	BOOST_REQUIRE(cache.problems().size() == 1u);

	// The edit reaches the static sibling through the skinned one: compiled right away, not through its own reload.
	desc->shininess = 7.f;
	const std::uint64_t serial = cache.get(plain).serial;
	next_frame(cache);
	cache.acquire(desc, true, true);
	BOOST_TEST(cache.get(plain).serial != serial);
	BOOST_TEST(cache.compilations() == compiled + 2);
	BOOST_TEST(cache.problems().size() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
