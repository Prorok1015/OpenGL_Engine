#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/texture/rnd_render_targets.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/scene/scn_material_desc.h>

#include <cstring>
#include <memory>

// The material instance cache on the null backend (RHI R6b, #125): an instance is compiled once, again only when
// its description changed, a texture it uses was reloaded, or a map was still loading, and is dropped after
// grace_frames frames without a user.

namespace {

using namespace rnd::driver;

struct instance_cache_fixture : test_util::null_engine_fixture
{
	rnd::texture_manager& textures() { return storage.require<rnd::render_system>().get_texture_manager(); }
	rnd::material_compiler_interface* compiler() { return storage.require<rnd::render_system>().get_material_compiler(); }

	std::shared_ptr<scn::material_desc> make_desc(float shininess = 32.f, std::vector<res::tag> samplers = {})
	{
		auto desc = std::make_shared<scn::material_desc>();
		desc->shininess = shininess;
		desc->vertex_shader = res::tag::make("shaders/scene.vert");
		desc->fragment_shader = res::tag::make("shaders/scene.frag");
		desc->samplers_textures = std::move(samplers);
		return desc;
	}

	// A new frame of the cache under test.
	void next_frame(rnd::material_instance_cache& cache) { cache.begin_frame(compiler()); }
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
	auto* first_map = rnd::render_targets::ensure(textures(), tag, { 8, 8 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(first_map);
	const auto desc = make_desc(32.f, { tag });

	next_frame(cache);
	const auto id = cache.acquire(desc, false, false);
	BOOST_REQUIRE(cache.get(id).complete);
	BOOST_REQUIRE(cache.get(id).runtime.samplers.size() == 1u);
	BOOST_TEST(cache.get(id).runtime.samplers[0] == first_map);
	const std::uint64_t serial = cache.get(id).serial;

	// Nothing changed: nothing compiled.
	next_frame(cache);
	BOOST_CHECK(cache.acquire(desc, false, false) == id);
	BOOST_TEST(cache.compilations() == 1u);

	// What a hot reload of the picture does to the texture manager: the texture is replaced, the generation moves.
	auto* second_map = rnd::render_targets::ensure(textures(), tag, { 16, 16 }, texture_header::TYPE::RGBA8, usage);
	BOOST_REQUIRE(second_map);
	next_frame(cache);
	BOOST_CHECK(cache.acquire(desc, false, false) == id);
	BOOST_TEST(cache.compilations() == 2u);
	BOOST_TEST(cache.get(id).serial != serial);
	BOOST_TEST(cache.get(id).runtime.samplers[0] == second_map);

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

BOOST_AUTO_TEST_SUITE_END()
