#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <snk/render/rnd_material_groups.h>
#include <snk/render/rnd_material_instance_cache.h>
#include <snk/render/shader/rnd_material_compiler.h>
#include <snk/render/texture/rnd_texture_manager.h>
#include <snk/resource/adapters/res_pct_adapter.h>
#include <snk/scene/scn_material_desc.h>
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace rnd::driver;
namespace json = boost::json;

struct texture_reload_fixture : test_util::null_engine_fixture
{
	texture_reload_fixture()
		: null_engine_fixture(false)
		, render(storage.require<rnd::render_system>())
		, gpu(dynamic_cast<null::driver&>(*render.get_driver()))
		, textures(render.get_texture_manager())
		, groups(gpu, textures)
		, cache(variants, textures)
	{
		gpu.observe_texture_uploads([this](texture_handle handle, const void* data, const texture_region& region) {
			BOOST_REQUIRE(gpu.info(handle).data.format == texture_header::TYPE::RGBA8);
			const auto* bytes = static_cast<const unsigned char*>(data);
			const std::size_t size = static_cast<std::size_t>(region.width) * region.height * region.depth * 4;
			uploads.push_back({ handle, region.face, std::vector<unsigned char>(bytes, bytes + size) });
		});
	}
	~texture_reload_fixture() { gpu.observe_texture_uploads({}); }

	res::resource_system& resources() { return storage.require<res::resource_system>(); }

	void picture(const res::tag& tag, unsigned char value)
	{
		res::raw_image_adapter::raw_image_header header{ { 1, 1 }, 4 };
		std::vector<std::byte> bytes(sizeof(header) + 4, static_cast<std::byte>(value));
		std::memcpy(bytes.data(), &header, sizeof(header));
		BOOST_REQUIRE(resources().store(tag, bytes));
	}

	void description(const res::tag& tag, const json::object& body)
	{
		BOOST_REQUIRE(resources().store(tag, test_util::to_bytes(json::serialize(body))));
	}

	std::shared_ptr<scn::material_desc> material(const res::tag& texture)
	{
		auto desc = std::make_shared<scn::material_desc>();
		desc->vertex_shader = res::tag::make("shaders/scene.vert");
		desc->fragment_shader = res::tag::make("shaders/scene.frag");
		desc->samplers_textures = { texture };
		return desc;
	}

	// A real material asks for its maps, then the queued resource work completes before the next frame.
	rnd::material_instance_id settle(const std::shared_ptr<scn::material_desc>& desc)
	{
		rnd::material_instance_id id;
		for (int frame = 0; frame < 10; ++frame) {
			test_util::pump(tasks);
			gpu.begin_frame();
			groups.begin_frame();
			cache.begin_frame(render.get_material_compiler());
			id = cache.acquire(desc, false, false);
			if (cache.get(id).complete) return id;
		}
		BOOST_FAIL("texture did not finish loading");
		return id;
	}

	bind_group_handle bind(rnd::material_instance_id id)
	{
		render_pass_desc pass;
		pass.backbuffer = true;
		pass.colors.push_back({});
		gpu.begin_render_pass(pass);
		groups.bind(cache.get(id));
		const auto group = gpu.bound(material_set).group;
		gpu.end_render_pass();
		return group;
	}

	void expect_pixels(texture_handle handle, std::uint32_t face, unsigned char value)
	{
		const auto found = std::find_if(uploads.rbegin(), uploads.rend(), [&](const upload& entry) {
			return entry.handle == handle && entry.face == face;
		});
		BOOST_REQUIRE_MESSAGE(found != uploads.rend(), "no pixel upload for the current GPU texture and face");
		BOOST_TEST((found->pixels == std::vector<unsigned char>(4, value)));
	}

	struct upload
	{
		texture_handle handle;
		std::uint32_t face;
		std::vector<unsigned char> pixels;
	};
	rnd::render_system& render;
	null::driver& gpu;
	rnd::texture_manager& textures;
	rnd::material_groups groups;
	rnd::shader_config_cache variants;
	rnd::material_instance_cache cache;
	std::vector<upload> uploads;
};
}

BOOST_FIXTURE_TEST_SUITE(RndTextureHotReloadTests, texture_reload_fixture)

BOOST_AUTO_TEST_CASE(PictureSignalReloadsUploadedPixelsAndMaterialGroups)
{
	const res::tag image{ "memory://reload/map.raw" };
	const res::tag texture{ "memory://reload/map.desc" };
	picture(image, 17);
	description(texture, { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) } });
	const auto desc = material(texture);
	const auto instance = settle(desc);
	const auto slot = textures.id_of(texture);
	const auto before = textures.binding(slot);
	const auto first_group = bind(instance);
	expect_pixels(before.handle, 0, 17);
	const auto* first = gpu.group_info(first_group);
	BOOST_REQUIRE(first);
	BOOST_CHECK(first->entries[1].texture == before.handle);

	picture(image, 91);
	resources().signal_changed(image);
	// Notifications must wait for the main thread, rather than touching the driver in signal_changed.
	BOOST_CHECK(textures.binding(slot).handle == before.handle);
	const auto again = settle(desc);
	BOOST_CHECK(again == instance);
	BOOST_CHECK(textures.id_of(texture) == slot);
	const auto after = textures.binding(slot);
	BOOST_TEST(after.generation != before.generation);
	expect_pixels(after.handle, 0, 91);
	const auto second_group = bind(again);
	BOOST_CHECK(second_group != first_group);
	BOOST_REQUIRE(gpu.group_info(second_group));
	BOOST_CHECK(gpu.group_info(second_group)->entries[1].texture == after.handle);
	const auto compiled = cache.compilations();
	settle(desc);
	BOOST_TEST(cache.compilations() == compiled);
	BOOST_CHECK(bind(instance) == second_group);
}

BOOST_AUTO_TEST_CASE(EveryCubemapFaceSignalReloadsTheCubeAndItsMaterialGroup)
{
	// These are the face indices used by the production cubemap loader, not JSON field order.
	const std::array<const char*, 6> names{ "right", "left", "bottom", "top", "front", "back" };
	std::array<res::tag, 6> faces;
	std::array<unsigned char, 6> values{};
	json::object data;
	for (std::size_t face = 0; face < faces.size(); ++face) {
		std::string path = "memory://reload/";
		path.append(names[face]).append(".raw");
		faces[face] = res::tag{ path };
		values[face] = static_cast<unsigned char>(10 + face);
		picture(faces[face], values[face]);
		data[names[face]] = path;
	}
	const res::tag texture{ "memory://reload/sky.desc" };
	description(texture, { { "__type", "texture_cubemap_desc" }, { "data", data } });
	const auto desc = material(texture);
	const auto instance = settle(desc);
	const auto slot = textures.id_of(texture);
	auto group = bind(instance);
	for (std::uint32_t changed = 0; changed < faces.size(); ++changed) {
		const auto before = textures.binding(slot);
		for (std::uint32_t face = 0; face < faces.size(); ++face) expect_pixels(before.handle, face, values[face]);
		values[changed] = static_cast<unsigned char>(100 + changed);
		picture(faces[changed], values[changed]);
		resources().signal_changed(faces[changed]);
		BOOST_CHECK(settle(desc) == instance);
		BOOST_CHECK(textures.id_of(texture) == slot);
		const auto after = textures.binding(slot);
		BOOST_TEST(after.generation != before.generation);
		for (std::uint32_t face = 0; face < faces.size(); ++face) expect_pixels(after.handle, face, values[face]);
		const auto fresh_group = bind(instance);
		BOOST_CHECK(fresh_group != group);
		BOOST_REQUIRE(gpu.group_info(fresh_group));
		BOOST_CHECK(gpu.group_info(fresh_group)->entries[1].texture == after.handle);
		group = fresh_group;
	}
}

BOOST_AUTO_TEST_CASE(SharedPictureAndDescChangesRefreshOnlyTheirCurrentUsers)
{
	const res::tag image{ "memory://reload/shared.raw" };
	const res::tag replacement{ "memory://reload/replacement.raw" };
	const res::tag first{ "memory://reload/first.desc" };
	const res::tag second{ "memory://reload/second.desc" };
	picture(image, 23);
	picture(replacement, 67);
	for (const auto& tag : { first, second })
		description(tag, { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) } });
	const auto a = material(first);
	const auto b = material(second);
	const auto instance_a = settle(a);
	const auto instance_b = settle(b);
	const auto slot_a = textures.id_of(first);
	const auto slot_b = textures.id_of(second);
	const auto before_a = textures.binding(slot_a);
	const auto before_b = textures.binding(slot_b);
	const auto group_a = bind(instance_a);
	const auto group_b = bind(instance_b);

	picture(image, 42);
	std::thread notifier([&] { resources().signal_changed(image); });
	notifier.join();
	BOOST_CHECK(textures.binding(slot_a).handle == before_a.handle);
	BOOST_CHECK(textures.binding(slot_b).handle == before_b.handle);
	settle(a);
	settle(b);
	expect_pixels(textures.binding(slot_a).handle, 0, 42);
	expect_pixels(textures.binding(slot_b).handle, 0, 42);
	BOOST_CHECK(bind(instance_a) != group_a);
	BOOST_CHECK(bind(instance_b) != group_b);

	// Changing a desc must update subscriptions as well as the GPU texture.
	description(first, { { "__type", "texture_2d_desc" }, { "data", std::string(replacement.view()) } });
	resources().signal_changed(first);
	settle(a);
	const auto replaced = textures.binding(slot_a);
	expect_pixels(replaced.handle, 0, 67);
	const auto replaced_group = bind(instance_a);
	const auto shared_before = textures.binding(slot_b);
	picture(image, 99);
	resources().signal_changed(image);
	settle(a);
	settle(b);
	BOOST_CHECK(textures.binding(slot_a).handle == replaced.handle);
	BOOST_TEST(textures.binding(slot_a).generation == replaced.generation);
	BOOST_CHECK(bind(instance_a) == replaced_group);
	BOOST_TEST(textures.binding(slot_b).generation != shared_before.generation);
	expect_pixels(textures.binding(slot_b).handle, 0, 99);
}

BOOST_AUTO_TEST_CASE(ParentDescChangeReachesTheLoadedTexture)
{
	const res::tag first{ "memory://reload/inherited_a.raw" };
	const res::tag second{ "memory://reload/inherited_b.raw" };
	const res::tag parent{ "memory://reload/parent.desc" };
	const res::tag child{ "memory://reload/child.desc" };
	picture(first, 18);
	picture(second, 72);
	description(parent, { { "__type", "texture_2d_desc" }, { "data", std::string(first.view()) } });
	description(child, { { "__type", "texture_2d_desc" }, { "__parent", std::string(parent.view()) } });
	const auto desc = material(child);
	const auto instance = settle(desc);
	const auto slot = textures.id_of(child);
	const auto group = bind(instance);
	expect_pixels(textures.binding(slot).handle, 0, 18);
	description(parent, { { "__type", "texture_2d_desc" }, { "data", std::string(second.view()) } });
	resources().signal_changed(parent);
	BOOST_CHECK(settle(desc) == instance);
	BOOST_CHECK(textures.id_of(child) == slot);
	expect_pixels(textures.binding(slot).handle, 0, 72);
	BOOST_CHECK(bind(instance) != group);
}

BOOST_AUTO_TEST_CASE(InheritedPicturesReloadEvenWhileTheParentDescIsHeld)
{
	for (bool cubemap : { false, true }) {
		const res::tag image{ cubemap ? "memory://reload/held_cube.raw" : "memory://reload/held_2d.raw" };
		const res::tag parent{ cubemap ? "memory://reload/held_cube.desc" : "memory://reload/held_2d.desc" };
		const res::tag child{ cubemap ? "memory://reload/child_cube.desc" : "memory://reload/child_2d.desc" };
		const char* type = cubemap ? "texture_cubemap_desc" : "texture_2d_desc";
		picture(image, 14);
		json::value data = std::string(image.view());
		if (cubemap) {
			json::object faces;
			for (const char* name : { "right", "left", "bottom", "top", "front", "back" }) faces[name] = data;
			data = std::move(faces);
		}
		description(parent, { { "__type", type }, { "data", data } });
		description(child, { { "__type", type }, { "__parent", std::string(parent.view()) } });
		const auto held_parent = resources().require_sync<rnd::texture_desc>(parent);
		BOOST_REQUIRE(held_parent.is_ready());
		const auto desc = material(child);
		const auto instance = settle(desc);
		const auto slot = textures.id_of(child);
		const auto before = textures.binding(slot);
		const auto group = bind(instance);
		for (std::uint32_t face = 0; face < (cubemap ? 6u : 1u); ++face) expect_pixels(before.handle, face, 14);
		picture(image, 83);
		resources().signal_changed(image);
		BOOST_CHECK(settle(desc) == instance);
		const auto after = textures.binding(slot);
		BOOST_TEST(after.generation != before.generation);
		for (std::uint32_t face = 0; face < (cubemap ? 6u : 1u); ++face) expect_pixels(after.handle, face, 83);
		BOOST_CHECK(bind(instance) != group);
	}
}

BOOST_AUTO_TEST_CASE(AFailedCubemapFaceCanBeRepairedByASourceSignal)
{
	const res::tag image{ "memory://reload/ready.raw" };
	const res::tag late{ "memory://reload/late.raw" };
	const res::tag texture{ "memory://reload/repaired_cube.desc" };
	picture(image, 64);
	json::object faces;
	for (const char* name : { "right", "left", "bottom", "top", "front", "back" })
		faces[name] = std::string(image.view());
	faces["front"] = std::string(late.view());
	description(texture, { { "__type", "texture_cubemap_desc" }, { "data", faces } });
	const auto desc = material(texture);
	cache.begin_frame(render.get_material_compiler());
	const auto instance = cache.acquire(desc, false, false);
	test_util::pump(tasks);
	cache.begin_frame(render.get_material_compiler());
	BOOST_CHECK(cache.acquire(desc, false, false) == instance);
	BOOST_TEST(!cache.get(instance).complete);
	const auto slot = textures.id_of(texture);
	BOOST_TEST(!textures.is_ready(slot));

	picture(late, 88);
	resources().signal_changed(late);
	BOOST_CHECK(settle(desc) == instance);
	const auto loaded = textures.binding(slot);
	for (std::uint32_t face = 0; face < 6; ++face) expect_pixels(loaded.handle, face, face == 4 ? 88 : 64);
	const auto group = bind(instance);
	BOOST_REQUIRE(gpu.group_info(group));
	BOOST_CHECK(gpu.group_info(group)->entries[1].texture == loaded.handle);
}

BOOST_AUTO_TEST_CASE(QueuedSourceNotificationDoesNotOutliveTheTextureManager)
{
	const res::tag image{ "memory://reload/lifetime.raw" };
	const res::tag texture{ "memory://reload/lifetime.desc" };
	picture(image, 12);
	description(texture, { { "__type", "texture_2d_desc" }, { "data", std::string(image.view()) } });
	const auto live = gpu.live_textures();
	{
		rnd::texture_manager temporary(&gpu, storage.require<desc::desc_system>(), resources());
		temporary.register_loader("texture_2d_desc", [](driver_interface* driver, const rnd::texture_desc& desc) {
			return make_texture(*driver, desc.get_header());
		});
		const auto slot = temporary.require(texture);
		test_util::pump(tasks);
		BOOST_CHECK(temporary.require(texture) == slot);
		BOOST_REQUIRE(temporary.is_ready(slot));
		resources().signal_changed(image);
	}
	test_util::pump(tasks); // a queued callback must not dereference the destroyed manager (ASan/TSan)
	BOOST_TEST(gpu.live_textures() == live);
}

BOOST_AUTO_TEST_SUITE_END()
