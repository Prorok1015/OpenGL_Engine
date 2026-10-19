#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include "rnd_frame_upload_pass.h"
#include <snk/common/mem_allocator.h>
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_per_draw_data.h>
#include <snk/render/rnd_render_packet.hpp>
#include <snk/render/skinning/rnd_skinning_manager.h>
#include <cstring>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <vector>

// The data a draw reads from set 3: the per-frame ring of ObjectData elements, the frame's bone matrices and
// the skin's index buffer, on the null backend (which keeps every copy of a per_frame buffer to look at).

namespace {

using namespace rnd::driver;

glm::mat4 at_x(float x)
{
	glm::mat4 m{ 1.0f };
	m[3][0] = x;
	return m;
}

rnd::object_data object_with(float x, std::uint32_t bone_offset = 0)
{
	rnd::object_data object;
	object.world = at_x(x);
	object.bone_offset = bone_offset;
	return object;
}

render_pass_desc backbuffer_pass()
{
	render_pass_desc pass;
	pass.backbuffer = true;
	pass.colors.push_back({});
	return pass;
}

struct draw_data_fixture
{
	draw_data_fixture()
	{
		ds::frame_allocator_reset();
		driver.record_bindings(true);
	}

	// Binds element `index` inside a pass of its own and returns the group that was bound.
	bind_group_handle bind(std::uint32_t index, const res::tag& skin = {}, const rnd::skinning_manager* skins = nullptr)
	{
		driver.begin_render_pass(backbuffer_pass());
		data.bind_object(index, skin, skins);
		const bind_group_handle group = driver.bound(object_set).group;
		driver.end_render_pass();
		return group;
	}

	buffer_handle ring() { return driver.group_info(bind(0))->entries[0].buffer; }
	buffer_handle bones() { return driver.group_info(bind(0))->entries[1].buffer; }

	rnd::object_data object(buffer_handle ring_buffer, std::size_t index, std::optional<std::uint32_t> copy = std::nullopt)
	{
		rnd::object_data result;
		const auto* bytes = driver.buffer_bytes(ring_buffer, copy);
		BOOST_REQUIRE(bytes);
		BOOST_REQUIRE((index + 1) * data.object_stride() <= bytes->size());
		std::memcpy(&result, bytes->data() + index * data.object_stride(), sizeof(result));
		return result;
	}

	null::driver driver;
	rnd::per_draw_data data{ driver };
};

}

BOOST_FIXTURE_TEST_SUITE(RndPerDrawDataTests, draw_data_fixture)

BOOST_AUTO_TEST_CASE(ElementsAreAlignedToTheUniformOffsetAlignment)
{
	BOOST_TEST(data.object_stride() == driver.uniform_offset_alignment());
	BOOST_TEST(data.object_stride() % driver.uniform_offset_alignment() == 0u);
	BOOST_TEST(data.object_stride() >= sizeof(rnd::object_data));
}

BOOST_AUTO_TEST_CASE(StrideRoundsUpToTheAlignmentOfAnyBackend)
{
	// A GPU with 64 byte alignment: 80 bytes of ObjectData take two alignment units.
	BOOST_TEST((sizeof(rnd::object_data) + 63) / 64 * 64 == 128u);
	BOOST_TEST((sizeof(rnd::object_data) + 255) / 256 * 256 == 256u);
	BOOST_TEST(sizeof(rnd::object_data) == 80u);
}

BOOST_AUTO_TEST_CASE(UploadWritesEveryElementAtItsStride)
{
	std::vector<rnd::object_data> objects;
	for (std::uint32_t i = 0; i < 5; ++i) {
		objects.push_back(object_with(static_cast<float>(i) + 0.5f, i * 3));
	}
	data.upload(objects, {});

	const buffer_handle buffer = ring();
	for (std::uint32_t i = 0; i < 5; ++i) {
		BOOST_TEST_CONTEXT("element " << i) {
			const rnd::object_data stored = object(buffer, i);
			BOOST_TEST(stored.world[3][0] == static_cast<float>(i) + 0.5f);
			BOOST_TEST(stored.world[0][0] == 1.0f);
			BOOST_TEST(stored.bone_offset == i * 3);
		}
	}
	// The padding between elements stays zero.
	const auto* bytes = driver.buffer_bytes(buffer);
	BOOST_REQUIRE(bytes);
	for (std::size_t i = sizeof(rnd::object_data); i < data.object_stride(); ++i) {
		BOOST_REQUIRE((*bytes)[i] == std::byte{ 0 });
	}
}

BOOST_AUTO_TEST_CASE(BindingAnObjectMovesTheDynamicOffsetOnly)
{
	data.upload(std::vector<rnd::object_data>(4), {});
	const auto groups_before = data.groups_created();
	const auto first = bind(0);
	const auto last = bind(3);
	BOOST_TEST((first == last));
	BOOST_TEST(data.groups_created() == groups_before);

	const auto& log = driver.bind_log();
	BOOST_REQUIRE(log.size() >= 2u);
	const auto& a = log[log.size() - 2];
	const auto& b = log[log.size() - 1];
	BOOST_TEST(a.set == object_set);
	BOOST_REQUIRE(a.dynamic_offsets.size() == 1u);
	BOOST_TEST(a.dynamic_offsets[0] == 0u);
	BOOST_REQUIRE(b.dynamic_offsets.size() == 1u);
	BOOST_TEST(b.dynamic_offsets[0] == 3 * data.object_stride());
	BOOST_TEST(b.dynamic_offsets[0] % driver.uniform_offset_alignment() == 0u);
}

BOOST_AUTO_TEST_CASE(TheGroupBindsAnObjectsWorthOfTheRingAtTheStartOfAnElement)
{
	const auto* info = driver.group_info(bind(0));
	BOOST_REQUIRE(info);
	BOOST_REQUIRE(info->entries.size() == 3u);
	BOOST_TEST((info->entries[0].type == binding_type::ubo));
	BOOST_TEST(info->entries[0].slot == rnd::bindings::object_slot);
	BOOST_TEST(info->entries[0].offset == 0u);
	BOOST_TEST(info->entries[0].size == sizeof(rnd::object_data));
	BOOST_TEST((info->entries[1].type == binding_type::ssbo));
	BOOST_TEST(info->entries[1].slot == rnd::bindings::bones_slot);
	BOOST_TEST(info->entries[1].size == data.bone_capacity() * sizeof(glm::mat4));
	BOOST_TEST((info->entries[2].type == binding_type::ssbo));
	BOOST_TEST(info->entries[2].slot == rnd::bindings::skin_slot);
}

BOOST_AUTO_TEST_CASE(RingAndBonesAreWrittenPerFrameInFlight)
{
	const buffer_handle buffer = ring();
	BOOST_TEST((driver.buffer_info(buffer)->update == buffer_update::per_frame));
	BOOST_TEST((driver.buffer_info(bones())->update == buffer_update::per_frame));
}

BOOST_AUTO_TEST_CASE(TheRingWrapsOverFramesInFlight)
{
	const buffer_handle buffer = ring();
	constexpr int frames = 8;
	for (int frame = 0; frame < frames; ++frame) {
		driver.begin_frame();
		data.upload(std::vector<rnd::object_data>(3, object_with(static_cast<float>(frame))), {});
		BOOST_TEST_CONTEXT("frame " << frame) {
			BOOST_TEST(driver.frame_slot() == static_cast<std::uint32_t>((frame + 1) % 3));
			BOOST_TEST(object(buffer, 0).world[3][0] == static_cast<float>(frame));
			BOOST_TEST(object(buffer, 2).world[3][0] == static_cast<float>(frame));
			// The copies of the two frames before still hold what those frames wrote.
			if (frame >= 2) {
				BOOST_TEST(object(buffer, 0, frame % 3).world[3][0] == static_cast<float>(frame - 1));
				BOOST_TEST(object(buffer, 0, (frame - 1) % 3).world[3][0] == static_cast<float>(frame - 2));
			}
		}
		driver.begin_render_pass(backbuffer_pass());
		data.bind_object(2, {}, nullptr);
		driver.end_render_pass();
		BOOST_TEST(driver.bind_log().back().frame_slot == driver.frame_slot());
		BOOST_TEST(driver.bind_log().back().dynamic_offsets[0] == 2 * data.object_stride());
	}
}

BOOST_AUTO_TEST_CASE(OffsetsRestartAtZeroEveryFrameAndStayWithinTheRing)
{
	std::size_t capacity_bytes = 0;
	for (int frame = 0; frame < 4; ++frame) {
		driver.begin_frame();
		data.upload(std::vector<rnd::object_data>(40), {});
		capacity_bytes = data.object_capacity() * data.object_stride();
		driver.begin_render_pass(backbuffer_pass());
		for (std::uint32_t i = 0; i < 40; ++i) {
			data.bind_object(i, {}, nullptr);
		}
		driver.end_render_pass();
	}
	const auto& log = driver.bind_log();
	BOOST_REQUIRE(log.size() == 4u * 40u);
	for (std::size_t i = 0; i < log.size(); ++i) {
		const std::uint32_t offset = log[i].dynamic_offsets[0];
		BOOST_TEST_CONTEXT("bind " << i) {
			BOOST_TEST(offset % driver.uniform_offset_alignment() == 0u);
			BOOST_TEST(offset + sizeof(rnd::object_data) <= capacity_bytes);
			BOOST_TEST(offset == (i % 40) * data.object_stride());
		}
	}
}

BOOST_AUTO_TEST_CASE(TheRingGrowsWhenAFrameDoesNotFitAndKeepsItsSizeAfterwards)
{
	const std::size_t initial = data.object_capacity();
	BOOST_TEST(initial >= 64u);
	const auto live_before = driver.live_buffers();

	data.upload(std::vector<rnd::object_data>(initial + 1, object_with(7.f)), {});
	BOOST_TEST(data.object_capacity() == 2 * initial);
	BOOST_TEST(driver.live_buffers() == live_before);	// the old ring is gone
	const buffer_handle buffer = ring();
	BOOST_TEST(object(buffer, initial).world[3][0] == 7.f);
	BOOST_TEST(driver.buffer_info(buffer)->size == 2 * initial * data.object_stride());

	const auto groups = data.groups_created();
	driver.begin_frame();
	data.upload(std::vector<rnd::object_data>(initial + 1), {});
	data.upload(std::vector<rnd::object_data>(initial / 2), {});
	BOOST_TEST(data.object_capacity() == 2 * initial);
	BOOST_TEST(data.groups_created() == groups);
}

BOOST_AUTO_TEST_CASE(BoneMatricesLiftTheOldLimitOf128)
{
	std::vector<glm::mat4> palette;
	for (int i = 0; i < 1000; ++i) {
		palette.push_back(at_x(static_cast<float>(i)));
	}
	data.upload(std::vector<rnd::object_data>(1), palette);
	BOOST_TEST(data.bone_capacity() >= 1000u);

	const buffer_handle buffer = bones();
	const auto* stored = driver.buffer_bytes(buffer);
	BOOST_REQUIRE(stored);
	BOOST_REQUIRE(stored->size() >= 1000 * sizeof(glm::mat4));
	glm::mat4 last;
	std::memcpy(&last, stored->data() + 999 * sizeof(glm::mat4), sizeof(last));
	BOOST_TEST(last[3][0] == 999.f);
}

BOOST_AUTO_TEST_CASE(EverySkinGetsItsIndexBufferInAGroupOfItsOwn)
{
	rnd::skinning_manager skins;
	const res::tag a = res::tag::make("skins/a");
	const res::tag b = res::tag::make("skins/b");
	skins.register_weights(a, { { 1, 2, 3 }, { 4 } });
	skins.register_weights(b, { { 9 } });

	const auto empty = bind(0);
	const auto groups_before = data.groups_created();
	const auto group_a = bind(0, a, &skins);
	const auto group_b = bind(0, b, &skins);
	BOOST_TEST((group_a != empty));
	BOOST_TEST((group_b != empty));
	BOOST_TEST((group_a != group_b));
	BOOST_TEST(data.groups_created() == groups_before + 2);

	// A second draw of the same skin reuses its group.
	BOOST_TEST((bind(1, a, &skins) == group_a));
	BOOST_TEST(data.groups_created() == groups_before + 2);

	// The buffer holds: column count, column offsets, then bone indices and weights of every column.
	const auto* info = driver.group_info(group_a);
	BOOST_REQUIRE(info);
	const auto* bytes = driver.buffer_bytes(info->entries[2].buffer);
	BOOST_REQUIRE(bytes);
	const std::vector<std::uint32_t> expected{ 2, 0, 3, 4, 1, 2, 3, 4 };
	BOOST_REQUIRE(bytes->size() == expected.size() * sizeof(std::uint32_t));
	BOOST_TEST(std::memcmp(bytes->data(), expected.data(), bytes->size()) == 0);
	BOOST_TEST(info->entries[2].size == bytes->size());
}

BOOST_AUTO_TEST_CASE(ASkinWithoutWeightsOrWithoutAManagerSeesTheEmptyBuffer)
{
	rnd::skinning_manager skins;
	const auto empty = bind(0);
	BOOST_TEST((bind(0, res::tag::make("skins/unknown"), &skins) == empty));
	BOOST_TEST((bind(0, res::tag::make("skins/unknown"), nullptr) == empty));
	BOOST_TEST((bind(0, res::tag{}, &skins) == empty));
}

BOOST_AUTO_TEST_CASE(GrowingTheRingRebuildsTheGroupsOfTheSkinsOnTheirNextUse)
{
	rnd::skinning_manager skins;
	const res::tag a = res::tag::make("skins/a");
	skins.register_weights(a, { { 1 } });
	const auto before = bind(0, a, &skins);
	data.upload(std::vector<rnd::object_data>(data.object_capacity() + 1), {});
	const auto after = bind(0, a, &skins);
	BOOST_TEST((before != after));
	const auto* info = driver.group_info(after);
	BOOST_REQUIRE(info);
	BOOST_TEST((info->entries[0].buffer == ring()));
}

BOOST_AUTO_TEST_CASE(NothingLeaksWhenTheDataIsDestroyed)
{
	rnd::skinning_manager skins;
	const res::tag a = res::tag::make("skins/a");
	skins.register_weights(a, { { 1 } });
	null::driver outliving;
	{
		rnd::per_draw_data scoped(outliving);
		outliving.begin_render_pass(backbuffer_pass());
		scoped.bind_object(0, a, &skins);
		outliving.end_render_pass();
		BOOST_TEST(outliving.live_buffers() > 0u);
		BOOST_TEST(outliving.live_bind_groups() > 0u);
	}
	BOOST_TEST(outliving.live_buffers() == 0u);
	BOOST_TEST(outliving.live_bind_groups() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- the palette of a packet and the frame upload ---

BOOST_AUTO_TEST_SUITE(RndBonePaletteTests)

BOOST_AUTO_TEST_CASE(ASkeletonIsCopiedOnceWhateverTheNumberOfDrawsUsingIt)
{
	ds::frame_allocator_reset();
	rnd::bone_palette_t palette;
	const std::vector<glm::mat4> a(3, at_x(1.f));
	const std::vector<glm::mat4> b(2, at_x(2.f));
	palette.reserve(a.size() + b.size(), 2);

	BOOST_TEST(palette.add(&a, a) == 0u);
	BOOST_TEST(palette.add(&b, b) == 3u);
	BOOST_TEST(palette.add(&a, a) == 0u);
	BOOST_TEST(palette.add(&b, b) == 3u);
	BOOST_TEST(palette.matrices.size() == 5u);
	BOOST_TEST(palette.matrices[2][3][0] == 1.f);
	BOOST_TEST(palette.matrices[3][3][0] == 2.f);
}

BOOST_AUTO_TEST_CASE(ReservedPaletteNeverReallocates)
{
	ds::frame_allocator_reset();
	rnd::bone_palette_t palette;
	const std::vector<glm::mat4> a(10, at_x(1.f));
	const std::vector<glm::mat4> b(20, at_x(2.f));
	palette.reserve(a.size() + b.size(), 2);
	const glm::mat4* data = palette.matrices.data();
	palette.add(&a, a);
	palette.add(&b, b);
	BOOST_TEST((palette.matrices.data() == data));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(RndFrameUploadPassTests, draw_data_fixture)

namespace {

rnd::draw_call_t skinned_draw(rnd::bone_palette_t& palette, const std::vector<glm::mat4>& skeleton, float x, const char* skin)
{
	rnd::draw_call_t draw;
	draw.transform = at_x(x);
	draw.skinning_tag = res::tag::make(skin);
	draw.bone_offset = palette.add(&skeleton, skeleton);
	return draw;
}

std::vector<glm::mat4> skeleton_of(std::size_t bones, float first)
{
	std::vector<glm::mat4> result;
	for (std::size_t i = 0; i < bones; ++i) {
		result.push_back(at_x(first + static_cast<float>(i)));
	}
	return result;
}

}

BOOST_AUTO_TEST_CASE(TwoSkinnedMeshesFindTheirOwnSkeletonInTheFrameBuffer)
{
	rnd::frame_context context;
	context.draw_data = &data;
	auto& packets = context.data.construct<std::pmr::vector<rnd::render_packet_t>>(ds::frame_allocator());

	const auto skeleton_a = skeleton_of(3, 100.f);
	const auto skeleton_b = skeleton_of(2, 200.f);
	const auto skeleton_c = skeleton_of(4, 300.f);

	rnd::render_packet_t first;
	first.bones.reserve(skeleton_a.size() + skeleton_b.size(), 2);
	first.opaque_draws.push_back(skinned_draw(first.bones, skeleton_a, 1.f, "skins/a"));
	first.opaque_draws.push_back(skinned_draw(first.bones, skeleton_b, 2.f, "skins/b"));
	first.transparent_draws.push_back(skinned_draw(first.bones, skeleton_a, 3.f, "skins/a"));
	rnd::draw_call_t static_draw;
	static_draw.transform = at_x(4.f);
	first.opaque_draws.push_back(static_draw);
	packets.push_back(std::move(first));

	rnd::render_packet_t second;
	second.bones.reserve(skeleton_c.size(), 1);
	second.opaque_draws.push_back(skinned_draw(second.bones, skeleton_c, 5.f, "skins/c"));
	packets.push_back(std::move(second));

	rnd::frame_upload_pass pass;
	pass.execute(context, driver);

	// Object ring: the opaque draws of a packet, then its transparent ones, then the next packet.
	BOOST_TEST(packets[0].object_base == 0u);
	BOOST_TEST(packets[1].object_base == 4u);
	BOOST_TEST(packets[0].opaque_object(1) == 1u);
	BOOST_TEST(packets[0].transparent_object(0) == 3u);
	BOOST_TEST(packets[1].opaque_object(0) == 4u);

	const buffer_handle objects = ring();
	BOOST_TEST(object(objects, 0).world[3][0] == 1.f);
	BOOST_TEST(object(objects, 0).bone_offset == 0u);	// skeleton A starts the palette
	BOOST_TEST(object(objects, 1).world[3][0] == 2.f);
	BOOST_TEST(object(objects, 1).bone_offset == 3u);	// skeleton B follows A's three matrices
	BOOST_TEST(object(objects, 2).world[3][0] == 4.f);
	BOOST_TEST(object(objects, 2).bone_offset == 0u);	// the draw without a skeleton
	BOOST_TEST(object(objects, 3).world[3][0] == 3.f);
	BOOST_TEST(object(objects, 3).bone_offset == 0u);	// the transparent draw shares skeleton A
	BOOST_TEST(object(objects, 4).world[3][0] == 5.f);
	BOOST_TEST(object(objects, 4).bone_offset == 5u);	// the next packet's palette starts after the first one's 5

	// The palette of the frame: the packets' palettes one after the other.
	BOOST_TEST(packets[1].bone_base == 5u);
	const auto* bytes = driver.buffer_bytes(bones());
	BOOST_REQUIRE(bytes);
	const float expected[] = { 100.f, 101.f, 102.f, 200.f, 201.f, 300.f, 301.f, 302.f, 303.f };
	for (std::size_t i = 0; i < std::size(expected); ++i) {
		glm::mat4 stored;
		std::memcpy(&stored, bytes->data() + i * sizeof(glm::mat4), sizeof(stored));
		BOOST_TEST_CONTEXT("matrix " << i) {
			BOOST_TEST(stored[3][0] == expected[i]);
		}
	}
}

BOOST_AUTO_TEST_CASE(NoPacketsMeansNoUploadAndNoFailure)
{
	rnd::frame_context context;
	context.draw_data = &data;
	const auto updates = driver.calls().buffer_updates;
	rnd::frame_upload_pass pass;
	BOOST_REQUIRE_NO_THROW(pass.execute(context, driver));
	BOOST_TEST(driver.calls().buffer_updates == updates);

	context.data.construct<std::pmr::vector<rnd::render_packet_t>>(ds::frame_allocator());
	BOOST_REQUIRE_NO_THROW(pass.execute(context, driver));
	BOOST_TEST(driver.calls().buffer_updates == updates);
}

BOOST_AUTO_TEST_SUITE_END()
