#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include <snk/render/rnd_bindings.h>
#include <snk/render/rnd_material_groups.h>
#include <snk/render/shader/rnd_material_params.h>
#include <snk/render/texture/rnd_texture_id.h>
#include <snk/driver/rnd_texture_owner.h>
#include <cstring>
#include <memory>
#include <vector>

// Set 2 on the null backend: the bind group of a material is made when it is first drawn and never written
// again, a material that is built anew (the description changed, a texture was reloaded) gets a group of its own,
// and the groups of materials nobody draws any more go away.

namespace {

using namespace rnd::driver;

render_pass_desc backbuffer_pass()
{
	render_pass_desc pass;
	pass.backbuffer = true;
	pass.colors.push_back({});
	return pass;
}

texture_header small_texture()
{
	texture_header header;
	header.data.extent.width = 4;
	header.data.extent.height = 4;
	return header;
}

rnd::material_params params_of(float shininess, glm::vec4 diffuse = glm::vec4{ 1.f })
{
	rnd::material_params params;
	params.shininess = shininess;
	params.diffuse_color = diffuse;
	return params;
}

// The slots of a texture manager, without one: a slot is a handle and a generation that grows when the handle changes.
struct fake_textures final : rnd::texture_resolver
{
	rnd::texture_id add(texture_handle handle)
	{
		slots.push_back({ handle, 1 });
		return rnd::texture_id{ static_cast<std::uint32_t>(slots.size()) };
	}

	// What a reload, a resize or the end of a load does to a slot.
	void replace(rnd::texture_id id, texture_handle handle)
	{
		slots[id.index - 1].handle = handle;
		++slots[id.index - 1].generation;
	}

	rnd::texture_binding binding(rnd::texture_id id) const override
	{
		return id.is_valid() && id.index <= slots.size() ? slots[id.index - 1] : rnd::texture_binding{};
	}

	std::vector<rnd::texture_binding> slots;
};

struct material_fixture
{
	material_fixture() { driver.record_bindings(true); }

	// A compiled material as the compiler leaves it, with the given params and textures.
	std::shared_ptr<rnd::compiled_material> material(const rnd::material_params& params, std::vector<rnd::texture_id> samplers = {})
	{
		auto result = std::make_shared<rnd::compiled_material>();
		result->runtime.samplers = std::move(samplers);
		result->runtime.material_params.resize(sizeof(rnd::material_params));
		std::memcpy(result->runtime.material_params.data(), &params, sizeof(params));
		return result;
	}

	// Binds the material in a pass of its own and returns the group that was bound at set 2.
	bind_group_handle bind(const rnd::compiled_material& m)
	{
		driver.begin_render_pass(backbuffer_pass());
		groups.bind(m);
		const bind_group_handle group = driver.bound(material_set).group;
		driver.end_render_pass();
		return group;
	}

	void next_frame()
	{
		driver.begin_frame();
		groups.begin_frame();
	}

	null::driver driver;
	fake_textures textures;
	rnd::material_groups groups{ driver, textures };
};

}

BOOST_FIXTURE_TEST_SUITE(RndMaterialGroupsTests, material_fixture)

BOOST_AUTO_TEST_CASE(TheLayoutHoldsTheParamsBlockAndThreeMaps)
{
	const auto* layout = driver.layout_info(groups.layout());
	BOOST_REQUIRE(layout);
	BOOST_REQUIRE(layout->entries.size() == 4u);
	BOOST_TEST((layout->entries[0].type == binding_type::ubo));
	BOOST_TEST(layout->entries[0].slot == rnd::bindings::material_params_slot);
	for (std::uint32_t slot = 0; slot < 3; ++slot) {
		BOOST_TEST((layout->entries[1 + slot].type == binding_type::texture));
		BOOST_TEST(layout->entries[1 + slot].slot == slot);
		BOOST_TEST(!layout->entries[1 + slot].dynamic);
	}
	// The engine's shaders read them where the table of the backend puts set 2: the block at 4, the maps at units 4-6.
	BOOST_TEST(*per_type_binding_table.resolve(material_set, rnd::bindings::material_params_slot, binding_type::ubo) == 4u);
	BOOST_TEST(*per_type_binding_table.resolve(material_set, rnd::bindings::diffuse_slot, binding_type::texture) == 4u);
	BOOST_TEST(*per_type_binding_table.resolve(material_set, rnd::bindings::specular_slot, binding_type::texture) == 5u);
	BOOST_TEST(*per_type_binding_table.resolve(material_set, rnd::bindings::normal_slot, binding_type::texture) == 6u);
}

BOOST_AUTO_TEST_CASE(TheGroupHoldsTheParamsAndTheTexturesOfTheMaterial)
{
	auto diffuse = make_texture(driver, small_texture());
	auto normal = make_texture(driver, small_texture());
	const auto diffuse_id = textures.add(diffuse.handle());
	const auto normal_id = textures.add(normal.handle());
	const auto m = material(params_of(12.f, glm::vec4{ 0.25f, 0.5f, 0.75f, 1.f }), { diffuse_id, rnd::texture_id{}, normal_id });

	const auto* info = driver.group_info(bind(*m));
	BOOST_REQUIRE(info);
	BOOST_REQUIRE(info->entries.size() == 4u);
	BOOST_TEST((info->layout == groups.layout()));

	const auto* bytes = driver.buffer_bytes(info->entries[0].buffer);
	BOOST_REQUIRE(bytes);
	BOOST_REQUIRE(bytes->size() == sizeof(rnd::material_params));
	BOOST_TEST(std::memcmp(bytes->data(), m->runtime.material_params.data(), bytes->size()) == 0);
	BOOST_TEST(info->entries[0].size == sizeof(rnd::material_params));
	BOOST_TEST((driver.buffer_info(info->entries[0].buffer)->usage == buffer_usage::uniform));

	BOOST_TEST((info->entries[1].texture == diffuse.handle()));
	BOOST_TEST((info->entries[3].texture == normal.handle()));
	// The specular map the material lacks is a texture of the renderer's, never null and never another material's.
	BOOST_TEST(static_cast<bool>(info->entries[2].texture));
	BOOST_TEST((info->entries[2].texture != diffuse.handle()));
	BOOST_TEST((info->entries[2].texture != normal.handle()));
}

BOOST_AUTO_TEST_CASE(AMaterialWithoutMapsGetsPlaceholdersForAllThree)
{
	const auto* info = driver.group_info(bind(*material(params_of(8.f))));
	BOOST_REQUIRE(info);
	BOOST_REQUIRE(info->entries.size() == 4u);
	for (std::size_t i = 1; i < 4; ++i) {
		BOOST_TEST(static_cast<bool>(info->entries[i].texture));
		for (std::size_t j = i + 1; j < 4; ++j) {
			BOOST_TEST((info->entries[i].texture != info->entries[j].texture));
		}
	}
}

BOOST_AUTO_TEST_CASE(AGroupIsMadeOnTheFirstDrawAndNothingIsWrittenAfterwards)
{
	const auto m = material(params_of(32.f));
	BOOST_TEST(groups.groups_created() == 0u);
	const auto created = driver.calls().bind_groups_created;
	const auto buffers = driver.calls().buffers_created;
	const auto updates = driver.calls().buffer_updates;

	const auto first = bind(*m);
	BOOST_TEST(groups.groups_created() == 1u);
	BOOST_TEST(driver.calls().bind_groups_created == created + 1);
	BOOST_TEST(driver.calls().buffers_created == buffers + 1);
	BOOST_TEST(driver.calls().buffer_updates == updates + 1);

	// Drawn again, in this frame and in the next ones: the same group, no new object, no write.
	for (int frame = 0; frame < 6; ++frame) {
		BOOST_TEST((bind(*m) == first));
		next_frame();
	}
	BOOST_TEST(groups.groups_created() == 1u);
	BOOST_TEST(driver.calls().bind_groups_created == created + 1);
	BOOST_TEST(driver.calls().buffers_created == buffers + 1);
	BOOST_TEST(driver.calls().buffer_updates == updates + 1);
	BOOST_TEST(driver.calls().bind_group_binds == 7u);	// but the bind itself is made for every draw
}

BOOST_AUTO_TEST_CASE(MaterialsOfTheSameContentsShareOneGroup)
{
	// The key is what the material holds, not the compiled material: a hundred entities with the same material
	// (each its own compiled_material) use one buffer and one group.
	const auto created = driver.calls().bind_groups_created;
	const auto buffers = driver.calls().buffers_created;
	std::vector<std::shared_ptr<rnd::compiled_material>> entities;
	for (int i = 0; i < 100; ++i) {
		entities.push_back(material(params_of(32.f)));
	}
	const auto first = bind(*entities.front());
	for (const auto& entity : entities) {
		BOOST_REQUIRE((bind(*entity) == first));
	}
	BOOST_TEST(groups.groups_created() == 1u);
	BOOST_TEST(groups.cached() == 1u);
	BOOST_TEST(driver.calls().bind_groups_created == created + 1);
	BOOST_TEST(driver.calls().buffers_created == buffers + 1);
}

BOOST_AUTO_TEST_CASE(AMaterialThatIsRebuiltEveryFrameWithTheSameContentsAllocatesNothing)
{
	// A texture that never loads: the extractor builds the material again at every extract.
	const auto created = driver.calls().bind_groups_created;
	for (int frame = 0; frame < 30; ++frame) {
		const auto rebuilt = material(params_of(32.f), { rnd::texture_id{} });
		bind(*rebuilt);
		next_frame();
	}
	BOOST_TEST(groups.groups_created() == 1u);
	BOOST_TEST(groups.cached() == 1u);
	BOOST_TEST(driver.calls().bind_groups_created == created + 1);
}

BOOST_AUTO_TEST_CASE(ABumpOfTheTextureGenerationWithUnchangedContentsCreatesNoGroup)
{
	// A render target was resized: every material is built again, with the textures it had.
	auto diffuse = make_texture(driver, small_texture());
	const auto diffuse_id = textures.add(diffuse.handle());
	const auto before = bind(*material(params_of(32.f), { diffuse_id }));
	next_frame();
	const auto after = bind(*material(params_of(32.f), { diffuse_id }));
	BOOST_TEST((after == before));
	BOOST_TEST(groups.groups_created() == 1u);
}

BOOST_AUTO_TEST_CASE(ARealChangeOfParamsOrOfATextureCreatesExactlyOneGroup)
{
	auto diffuse = make_texture(driver, small_texture());
	auto other = make_texture(driver, small_texture());
	const auto diffuse_id = textures.add(diffuse.handle());
	const auto other_id = textures.add(other.handle());
	bind(*material(params_of(32.f), { diffuse_id }));
	BOOST_TEST(groups.groups_created() == 1u);

	bind(*material(params_of(64.f), { diffuse_id }));
	BOOST_TEST(groups.groups_created() == 2u);
	bind(*material(params_of(64.f), { other_id }));
	BOOST_TEST(groups.groups_created() == 3u);
	// Back to something seen: found, not made.
	bind(*material(params_of(32.f), { diffuse_id }));
	BOOST_TEST(groups.groups_created() == 3u);
	BOOST_TEST(groups.cached() == 3u);
}

BOOST_AUTO_TEST_CASE(ASlotWhoseTextureWasReloadedGetsANewGroupWithTheNewTexture)
{
	auto old_texture = make_texture(driver, small_texture());
	const auto slot = textures.add(old_texture.handle());
	const auto m = material(params_of(32.f), { slot });
	const auto group_before = bind(*m);
	BOOST_REQUIRE(driver.group_info(group_before));
	BOOST_TEST((driver.group_info(group_before)->entries[1].texture == old_texture.handle()));

	// The reload: the id stays, the texture behind it is another one and the generation of the slot moves.
	// The material itself is the same object; it is not built again.
	auto new_texture = make_texture(driver, small_texture());
	textures.replace(slot, new_texture.handle());
	next_frame();
	const auto group_after = bind(*m);
	BOOST_TEST((group_after != group_before));
	BOOST_TEST((driver.group_info(group_after)->entries[1].texture == new_texture.handle()));
	BOOST_TEST(groups.groups_created() == 2u);

	// And a frame after it nothing is made again.
	next_frame();
	BOOST_TEST((bind(*m) == group_after));
	BOOST_TEST(groups.groups_created() == 2u);
}

BOOST_AUTO_TEST_CASE(ASlotThatIsStillLoadingReadsAsThePlaceholderUntilItsTextureArrives)
{
	const auto slot = textures.add(texture_handle{});	// the id exists, the texture is not there yet
	const auto m = material(params_of(32.f), { slot });
	const auto loading = bind(*m);
	const auto placeholder = driver.group_info(loading)->entries[1].texture;
	BOOST_TEST(static_cast<bool>(placeholder));
	BOOST_TEST((bind(*material(params_of(32.f))) == loading));	// the same as a material without the map

	auto arrived = make_texture(driver, small_texture());
	textures.replace(slot, arrived.handle());
	next_frame();
	const auto loaded = bind(*m);
	BOOST_TEST((loaded != loading));
	BOOST_TEST((driver.group_info(loaded)->entries[1].texture == arrived.handle()));
}

BOOST_AUTO_TEST_CASE(TheGroupsOfMaterialsThatWereNotDrawnForAFrameAreDestroyed)
{
	const auto kept = material(params_of(1.f));
	const auto dropped = material(params_of(2.f));
	bind(*kept);
	const auto dropped_group = bind(*dropped);
	const auto live_groups = driver.live_bind_groups();
	const auto live_buffers = driver.live_buffers();
	BOOST_TEST(groups.cached() == 2u);

	// `dropped` stays for the grace period, drawn or not...
	for (std::uint64_t frame = 1; frame <= rnd::material_groups::grace_frames; ++frame) {
		next_frame();
		bind(*kept);
		BOOST_TEST(groups.cached() == 2u);
	}
	// ...and goes with the next frame.
	next_frame();
	bind(*kept);
	BOOST_TEST(groups.cached() == 1u);
	BOOST_TEST(driver.live_bind_groups() == live_groups - 1);
	BOOST_TEST(driver.live_buffers() == live_buffers - 1);
	BOOST_TEST(driver.group_info(dropped_group) == nullptr);
}

BOOST_AUTO_TEST_CASE(AGroupUsedAgainWithinTheGraceIsNotMadeAgain)
{
	const auto m = material(params_of(5.f));
	const auto first = bind(*m);
	for (std::uint64_t frame = 0; frame < rnd::material_groups::grace_frames; ++frame) {
		next_frame();
	}
	BOOST_TEST((bind(*m) == first));
	BOOST_TEST(groups.groups_created() == 1u);
}

BOOST_AUTO_TEST_CASE(ParamsThatAreNotTheSizeOfTheBlockAreCopiedAsFarAsTheyGoAndTheRestKeepsItsDefaults)
{
	auto m = std::make_shared<rnd::compiled_material>();
	const auto* info = driver.group_info(bind(*m));	// no params at all
	BOOST_REQUIRE(info);
	const auto* bytes = driver.buffer_bytes(info->entries[0].buffer);
	BOOST_REQUIRE(bytes);
	rnd::material_params stored;
	std::memcpy(&stored, bytes->data(), sizeof(stored));
	BOOST_TEST(stored.shininess == rnd::material_params{}.shininess);
	BOOST_TEST((stored.diffuse_color == rnd::material_params{}.diffuse_color));
}

BOOST_AUTO_TEST_CASE(NothingIsLeftWhenTheGroupsAreDestroyed)
{
	null::driver outliving;
	const auto m = material(params_of(32.f));
	{
		rnd::material_groups scoped(outliving, textures);
		outliving.begin_render_pass(backbuffer_pass());
		scoped.bind(*m);
		outliving.end_render_pass();
		BOOST_TEST(outliving.live_bind_groups() == 1u);
		BOOST_TEST(outliving.live_buffers() == 1u);
	}
	BOOST_TEST(outliving.live_bind_groups() == 0u);
	BOOST_TEST(outliving.live_buffers() == 0u);
	BOOST_TEST(outliving.stats().textures.live() == 0u);	// the placeholders went with them
}

BOOST_AUTO_TEST_SUITE_END()
