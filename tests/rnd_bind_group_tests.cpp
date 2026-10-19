#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include "rnd_null_swapchain.h"
#include "rnd_validation_driver.h"
#include <snk/render/rnd_bindings.h>
#include <snk/render/shader/rnd_shader_bindings.h>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Bind groups, buffers by handle and the (set, slot, type) binding table: the contract of the null backend, the
// rules of the validation decorator (one wrong call sequence per rule, and a correct one that passes), and the
// expansion of the SNK_* macros the shader preprocessor does with the table.

namespace {

using namespace rnd::driver;
namespace vl = rnd::driver::validation;

std::vector<shader_header> program()
{
	shader_header vertex;
	vertex.title = "a.vert";
	vertex.body = "#version 450 core\nvoid main() {}\n";
	vertex.type = shader_header::shader_type::VERTEX;
	shader_header fragment = vertex;
	fragment.title = "a.frag";
	fragment.type = shader_header::shader_type::FRAGMENT;
	return { vertex, fragment };
}

texture_header texture_of(texture_usage usage)
{
	texture_header header;
	header.data.extent.width = 16;
	header.data.extent.height = 16;
	header.usage = usage;
	return header;
}

render_pass_desc backbuffer_pass()
{
	render_pass_desc pass;
	pass.backbuffer = true;
	pass.colors.push_back({});
	pass.debug_name = "test";
	return pass;
}

bind_group_layout_desc layout_of(std::initializer_list<binding_decl> decls)
{
	bind_group_layout_desc desc;
	for (const binding_decl& decl : decls) {
		desc.entries.push_back(decl);
	}
	return desc;
}

bind_group_desc group_of(bind_group_layout_handle layout, std::initializer_list<binding_res> entries)
{
	bind_group_desc desc;
	desc.layout = layout;
	for (const binding_res& entry : entries) {
		desc.entries.push_back(entry);
	}
	return desc;
}

binding_res ubo_entry(std::uint32_t slot, buffer_handle buffer, std::size_t size, std::size_t offset = 0)
{
	return { binding_type::ubo, slot, buffer, offset, size, nullptr };
}

binding_res ssbo_entry(std::uint32_t slot, buffer_handle buffer, std::size_t size)
{
	return { binding_type::ssbo, slot, buffer, 0, size, nullptr };
}

binding_res ssbo_entry_at(std::uint32_t slot, buffer_handle buffer, std::size_t size, std::size_t offset)
{
	return { binding_type::ssbo, slot, buffer, offset, size, nullptr };
}

binding_res texture_entry(std::uint32_t slot, texture_handle texture)
{
	return { binding_type::texture, slot, {}, 0, 0, texture };
}

int read_int(const std::vector<std::byte>* bytes, std::size_t offset = 0)
{
	int value = 0;
	std::memcpy(&value, bytes->data() + offset, sizeof(value));
	return value;
}

std::span<const std::byte> bytes_of(const int& value)
{
	return std::as_bytes(std::span{ &value, 1 });
}

struct geometry
{
	std::unique_ptr<vertex_array_interface> array;
	std::shared_ptr<buffer_interface> vertices;
	std::shared_ptr<buffer_interface> indices;
};

geometry make_geometry(driver_interface& drv)
{
	geometry result;
	result.array = drv.create_vertex_array();
	result.vertices = drv.create_buffer();
	result.vertices->set_layout({ { SHADER_DATA_TYPE::VEC3_F, "position" } });
	result.vertices->set_data(std::vector<float>(9));
	result.array->add_vertex_buffer(result.vertices);
	result.indices = drv.create_buffer();
	result.indices->set_data(std::vector<unsigned int>(3));
	result.array->set_index_buffer(result.indices);
	return result;
}

}

BOOST_AUTO_TEST_SUITE(RndBindGroupTests)

// --- the binding table ---

BOOST_AUTO_TEST_CASE(BindingsAreNumberedPerResourceTypeAndNotBySetTimesSixteen)
{
	const binding_table& table = per_type_binding_table;
	// Each set owns a block of the bindings of a type, in the order of the sets.
	BOOST_TEST(*table.resolve(frame_set, 0, binding_type::ubo) == 0u);
	BOOST_TEST(*table.resolve(frame_set, 1, binding_type::ubo) == 1u);
	BOOST_TEST(*table.resolve(pass_set, 0, binding_type::ubo) == 2u);
	BOOST_TEST(*table.resolve(material_set, 0, binding_type::ubo) == 4u);
	BOOST_TEST(*table.resolve(object_set, 0, binding_type::ubo) == 6u);
	BOOST_TEST(*table.resolve(object_set, 0, binding_type::ssbo) == 0u);
	BOOST_TEST(*table.resolve(object_set, 1, binding_type::ssbo) == 1u);
	BOOST_TEST(*table.resolve(pass_set, 0, binding_type::texture) == 0u);
	BOOST_TEST(*table.resolve(pass_set, 3, binding_type::texture) == 3u);
	BOOST_TEST(*table.resolve(material_set, 0, binding_type::texture) == 4u);
	BOOST_TEST(*table.resolve(material_set, 7, binding_type::texture) == 11u);
	BOOST_TEST(*table.resolve(object_set, 0, binding_type::ubo) != object_set * 16u);
}

BOOST_AUTO_TEST_CASE(BindingsOutsideTheTableHaveNoNumber)
{
	const binding_table& table = per_type_binding_table;
	BOOST_TEST(!table.resolve(4, 0, binding_type::ubo).has_value());
	BOOST_TEST(!table.resolve(frame_set, 2, binding_type::ubo).has_value());
	BOOST_TEST(!table.resolve(frame_set, 0, binding_type::ssbo).has_value());
	BOOST_TEST(!table.resolve(object_set, 2, binding_type::ssbo).has_value());
	BOOST_TEST(!table.resolve(pass_set, 4, binding_type::texture).has_value());
	BOOST_TEST(!table.resolve(object_set, 0, binding_type::texture).has_value());
	BOOST_TEST(!table.contains(max_bind_sets, 0, binding_type::ubo));
}

BOOST_AUTO_TEST_CASE(TheTableStaysWithinWhatGlGuarantees)
{
	const binding_table& table = per_type_binding_table;
	// GL guarantees 8 storage buffer bindings, 36 uniform buffer bindings and 16 texture units per stage.
	BOOST_TEST(table.total(binding_type::ssbo) <= 8u);
	BOOST_TEST(table.total(binding_type::ubo) <= 36u);
	BOOST_TEST(table.total(binding_type::texture) <= 16u);
}

BOOST_AUTO_TEST_CASE(NullAndValidationDriversHandOutTheTableOfTheirBackend)
{
	// By value: the table is an inline constant, a shared engine library has a copy of its own.
	const auto same_table = [](const binding_table& table) {
		for (std::uint32_t set = 0; set <= max_bind_sets; ++set) {
			for (std::uint32_t slot = 0; slot < 16; ++slot) {
				for (auto type : { binding_type::ubo, binding_type::ssbo, binding_type::texture }) {
					if (table.resolve(set, slot, type) != per_type_binding_table.resolve(set, slot, type)) {
						return false;
					}
				}
			}
		}
		return true;
	};

	null::driver driver;
	BOOST_TEST(same_table(driver.bindings()));
	BOOST_TEST(driver.uniform_offset_alignment() == 256u);

	vl::driver validating(std::make_unique<null::driver>());
	BOOST_TEST(same_table(validating.bindings()));
	BOOST_TEST(validating.uniform_offset_alignment() == 256u);
}

// --- the engine's layouts follow the table ---

BOOST_AUTO_TEST_CASE(EngineLayoutsFitTheBindingTable)
{
	const binding_table& table = per_type_binding_table;
	const auto fits = [&](const bind_group_layout_desc& layout, std::uint32_t set) {
		for (const binding_decl& decl : layout.entries) {
			if (!table.contains(set, decl.slot, decl.type)) {
				return false;
			}
		}
		return true;
	};
	BOOST_TEST(fits(rnd::frame_layout_desc(), frame_set));
	BOOST_TEST(fits(rnd::composition_layout_desc(), pass_set));
	BOOST_TEST(fits(rnd::sky_layout_desc(), pass_set));
	BOOST_TEST(fits(rnd::object_layout_desc(), object_set));
}

// --- the null backend ---

BOOST_AUTO_TEST_CASE(LayoutsAreSharedByTheirEntries)
{
	null::driver driver;
	const auto a = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto b = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto c = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 1 } }));
	const auto d = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 0, shader_stage::vertex, true } }));
	BOOST_TEST((a == b));
	BOOST_TEST((a != c));
	BOOST_TEST((a != d));
	BOOST_TEST(driver.layout_count() == 3u);
}

BOOST_AUTO_TEST_CASE(BuffersKeepTheirDescriptionAndBytes)
{
	null::driver driver;
	const buffer_handle buffer = driver.create_buffer({ .size = 32, .usage = buffer_usage::storage, .update = buffer_update::static_data });
	BOOST_REQUIRE(static_cast<bool>(buffer));
	BOOST_TEST((driver.buffer_info(buffer)->usage == buffer_usage::storage));
	BOOST_TEST(driver.buffer_bytes(buffer)->size() == 32u);

	driver.update_buffer(buffer, 4, bytes_of(7));
	BOOST_TEST(read_int(driver.buffer_bytes(buffer), 4) == 7);
	BOOST_TEST(driver.calls().buffers_created == 1u);
	BOOST_TEST(driver.calls().buffer_updates == 1u);

	driver.destroy_buffer(buffer);
	BOOST_TEST(driver.buffer_info(buffer) == nullptr);
	BOOST_TEST(driver.buffer_bytes(buffer) == nullptr);
}

BOOST_AUTO_TEST_CASE(AReusedBufferSlotNeverMatchesTheOldHandle)
{
	null::driver driver;
	const buffer_handle first = driver.create_buffer({ .size = 16 });
	driver.destroy_buffer(first);
	const buffer_handle second = driver.create_buffer({ .size = 16 });
	BOOST_TEST(first.index == second.index);
	BOOST_TEST(first.gen != second.gen);
	BOOST_TEST(driver.buffer_info(first) == nullptr);
	BOOST_TEST(driver.buffer_info(second) != nullptr);
}

BOOST_AUTO_TEST_CASE(PerFrameBuffersKeepACopyPerFrameInFlightAndRotateAtBeginFrame)
{
	null::driver driver;
	const buffer_handle buffer = driver.create_buffer({ .size = 4, .update = buffer_update::per_frame });

	// Frames 0, 1, 2 fill the three copies; frame 3 comes back to the first.
	driver.update_buffer(buffer, 0, bytes_of(10));
	driver.begin_frame();
	driver.update_buffer(buffer, 0, bytes_of(11));
	driver.begin_frame();
	driver.update_buffer(buffer, 0, bytes_of(12));
	driver.begin_frame();
	BOOST_TEST(driver.frame_slot() == 0u);
	BOOST_TEST(read_int(driver.buffer_bytes(buffer, 0)) == 10);
	driver.update_buffer(buffer, 0, bytes_of(13));

	BOOST_TEST(read_int(driver.buffer_bytes(buffer, 0)) == 13);
	BOOST_TEST(read_int(driver.buffer_bytes(buffer, 1)) == 11);
	BOOST_TEST(read_int(driver.buffer_bytes(buffer, 2)) == 12);
	BOOST_TEST(read_int(driver.buffer_bytes(buffer)) == 13);
}

BOOST_AUTO_TEST_CASE(OtherBuffersHaveOneCopy)
{
	null::driver driver;
	const buffer_handle buffer = driver.create_buffer({ .size = 4, .update = buffer_update::dynamic });
	driver.update_buffer(buffer, 0, bytes_of(5));
	driver.begin_frame();
	driver.begin_frame();
	BOOST_TEST(read_int(driver.buffer_bytes(buffer)) == 5);
	BOOST_TEST(read_int(driver.buffer_bytes(buffer, 2)) == 5);
}

BOOST_AUTO_TEST_CASE(TransientGroupsEndWithTheirFramePersistentOnesDoNot)
{
	null::driver driver;
	const auto layout = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const buffer_handle buffer = driver.create_buffer({ .size = 64 });
	const bind_group_desc desc = group_of(layout, { ubo_entry(0, buffer, 64) });

	const auto persistent = driver.create_bind_group(desc, lifetime::persistent);
	const auto transient = driver.create_bind_group(desc, lifetime::transient);
	BOOST_TEST(driver.live_bind_groups() == 2u);
	BOOST_TEST(driver.calls().bind_groups_created == 1u);
	BOOST_TEST(driver.calls().transient_bind_groups_created == 1u);
	BOOST_TEST(driver.group_info(transient) != nullptr);

	driver.begin_frame();
	BOOST_TEST(driver.group_info(transient) == nullptr);
	BOOST_TEST(driver.group_info(persistent) != nullptr);
	BOOST_TEST(driver.live_bind_groups() == 1u);

	driver.destroy_bind_group(persistent);
	BOOST_TEST(driver.live_bind_groups() == 0u);
}

BOOST_AUTO_TEST_CASE(SetBindGroupKeepsTheGroupAndTheDynamicOffsetsUntilThePassEnds)
{
	null::driver driver;
	driver.record_bindings(true);
	const auto layout = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 0, shader_stage::vertex, true } }));
	const buffer_handle buffer = driver.create_buffer({ .size = 1024, .update = buffer_update::per_frame });
	const auto group = driver.create_bind_group(group_of(layout, { ubo_entry(0, buffer, 80) }));

	driver.begin_render_pass(backbuffer_pass());
	const std::uint32_t offsets[] = { 512 };
	driver.set_bind_group(object_set, group, offsets);
	BOOST_TEST((driver.bound(object_set).group == group));
	BOOST_REQUIRE(driver.bound(object_set).dynamic_offsets.size() == 1u);
	BOOST_TEST(driver.bound(object_set).dynamic_offsets[0] == 512u);
	BOOST_TEST(!driver.bound(frame_set).group);
	driver.end_render_pass();

	driver.begin_render_pass(backbuffer_pass());
	BOOST_TEST(!driver.bound(object_set).group);
	driver.end_render_pass();

	BOOST_REQUIRE(driver.bind_log().size() == 1u);
	BOOST_TEST(driver.bind_log()[0].set == object_set);
	BOOST_TEST(driver.bind_log()[0].frame_slot == 0u);
	BOOST_TEST(driver.calls().bind_group_binds == 1u);
}

BOOST_AUTO_TEST_CASE(DrawsCountTheSetsTheyHaveBound)
{
	null::driver driver;
	auto geom = make_geometry(driver);
	const auto layout = driver.create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const buffer_handle buffer = driver.create_buffer({ .size = 64 });
	const auto group = driver.create_bind_group(group_of(layout, { ubo_entry(0, buffer, 64) }));

	driver.begin_render_pass(backbuffer_pass());
	driver.draw_indices(geom.array.get(), 3, 0, 0);
	driver.set_bind_group(frame_set, group);
	driver.draw_indices(geom.array.get(), 3, 0, 0);
	driver.draw_indices(geom.array.get(), 3, 0, 0);
	driver.end_render_pass();

	BOOST_TEST(driver.calls().draws == 3u);
	BOOST_TEST(driver.draws_with_set()[frame_set] == 2u);
	BOOST_TEST(driver.draws_with_set()[object_set] == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- the validation decorator ---

namespace {

struct bind_fixture
{
	bind_fixture(vl::limits budget = {})
		: chain(std::make_unique<null::swapchain>(), budget)
		, drv(chain.create_driver())
	{
		vertex_layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0, shader_stage::vertex, true } }));
	}

	vl::validator& state() { return chain.state(); }
	null::driver& inner() { return dynamic_cast<null::driver&>(dynamic_cast<vl::driver&>(*drv).inner()); }

	void open_pass()
	{
		if (!state().in_pass()) {
			drv->begin_render_pass(backbuffer_pass());
		}
	}

	// A pipeline for the backbuffer pass that expects `layouts` at its sets.
	pipeline_handle pipeline_with(std::initializer_list<bind_group_layout_handle> layouts)
	{
		program_object = drv->create_shader(program());
		pipeline_desc desc;
		desc.shader = program_object.get();
		desc.color_formats.push_back(backbuffer_color_format);
		desc.depth_format = backbuffer_depth_format;
		for (const auto& layout : layouts) {
			desc.set_layouts.push_back(layout);
		}
		return drv->create_pipeline(desc);
	}

	void expect_only(vl::rule kind, std::size_t count = 1)
	{
		BOOST_TEST(state().total() == count);
		BOOST_TEST(state().count(kind) == count);
	}

	vl::swapchain chain;
	std::unique_ptr<driver_interface> drv;
	std::unique_ptr<shader_interface> program_object;
	bind_group_layout_handle vertex_layout;
};

}

BOOST_FIXTURE_TEST_SUITE(RndBindGroupValidationTests, bind_fixture)

BOOST_AUTO_TEST_CASE(ACorrectUseOfBindGroupsProducesNoViolations)
{
	const buffer_handle ring = drv->create_buffer({ .size = 1024, .usage = buffer_usage::uniform, .update = buffer_update::per_frame });
	const buffer_handle bones = drv->create_buffer({ .size = 256, .usage = buffer_usage::storage });
	auto texture = drv->create_texture(texture_of(texture_usage::sampled));
	const auto object_layout = drv->create_bind_group_layout(layout_of({
		{ binding_type::ubo, 0, shader_stage::vertex, true }, { binding_type::ssbo, 0, shader_stage::vertex, false } }));
	const auto pass_layout = drv->create_bind_group_layout(layout_of({ { binding_type::texture, 0, shader_stage::fragment, false } }));
	const auto objects = drv->create_bind_group(group_of(object_layout, { ubo_entry(0, ring, 80), ssbo_entry(0, bones, 256) }));
	auto geom = make_geometry(*drv);
	const pipeline_handle pipeline = pipeline_with({ {}, pass_layout, {}, object_layout });

	for (int frame = 0; frame < 3; ++frame) {
		drv->begin_frame();
		drv->update_buffer(ring, 0, bytes_of(frame));
		open_pass();
		drv->bind_pipeline(pipeline);
		const auto inputs = drv->create_bind_group(group_of(pass_layout, { texture_entry(0, texture.get()) }), lifetime::transient);
		drv->set_bind_group(pass_set, inputs);
		for (std::uint32_t draw = 0; draw < 3; ++draw) {
			const std::uint32_t offset = draw * 256;
			drv->set_bind_group(object_set, objects, std::span{ &offset, 1 });
			drv->draw_indices(geom.array.get(), 3, 0, 0);
		}
		drv->end_render_pass();
		chain.present();
	}

	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(inner().calls().bind_group_binds == 12u);
	BOOST_TEST(inner().calls().draws == 9u);
	drv->begin_frame();
	BOOST_TEST(inner().live_bind_groups() == 1u);	// the transient groups ended with their frames
}

BOOST_AUTO_TEST_CASE(ALayoutSlotOutsideTheTableIsRejected)
{
	// No set has a third uniform block binding.
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 2 } }));
	BOOST_TEST(!layout);
	expect_only(vl::rule::binding_outside_table);
	BOOST_TEST(inner().layout_count() == 1u);	// only the fixture's own

	drv->create_bind_group_layout(layout_of({ { binding_type::ssbo, 2 } }));
	drv->create_bind_group_layout(layout_of({ { binding_type::texture, 8 } }));
	BOOST_TEST(state().count(vl::rule::binding_outside_table) == 3u);
}

BOOST_AUTO_TEST_CASE(ASetPastTheTableIsRejectedAndNotForwarded)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	const auto group = drv->create_bind_group(group_of(vertex_layout, { ubo_entry(0, buffer, 64) }));
	open_pass();
	const std::uint32_t offset = 0;

	drv->set_bind_group(max_bind_sets, group, std::span{ &offset, 1 });
	expect_only(vl::rule::binding_outside_table);
	BOOST_TEST(inner().calls().bind_group_binds == 0u);

	drv->set_bind_group(object_set, group, std::span{ &offset, 1 });
	BOOST_TEST(state().total() == 1u);
	BOOST_TEST(inner().calls().bind_group_binds == 1u);
}

BOOST_AUTO_TEST_CASE(ASlotThatTheSetDoesNotHaveIsRejected)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64, .usage = buffer_usage::storage });
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ssbo, 0 } }));
	const auto group = drv->create_bind_group(group_of(layout, { ssbo_entry(0, buffer, 64) }));
	open_pass();

	// The frame set has no storage buffer bindings, the object set has two.
	drv->set_bind_group(frame_set, group);
	expect_only(vl::rule::binding_outside_table);
	BOOST_TEST(inner().calls().bind_group_binds == 0u);
	drv->set_bind_group(object_set, group);
	BOOST_TEST(state().total() == 1u);
	BOOST_TEST(inner().calls().bind_group_binds == 1u);
}

BOOST_AUTO_TEST_CASE(AGroupThatDoesNotMatchItsLayoutIsRejected)
{
	const buffer_handle uniform = drv->create_buffer({ .size = 64, .usage = buffer_usage::uniform });
	const buffer_handle storage = drv->create_buffer({ .size = 64, .usage = buffer_usage::storage });
	auto sampled = drv->create_texture(texture_of(texture_usage::sampled));
	auto target_only = drv->create_texture(texture_of(texture_usage::color_target));
	const auto two = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 }, { binding_type::ubo, 1 } }));
	const auto one = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto tex = drv->create_bind_group_layout(layout_of({ { binding_type::texture, 0 } }));

	// an entry is missing
	BOOST_TEST(!drv->create_bind_group(group_of(two, { ubo_entry(0, uniform, 64) })));
	expect_only(vl::rule::bind_group_layout_mismatch, 2);	// the count and the missing slot
	BOOST_TEST(inner().live_bind_groups() == 0u);

	// an entry the layout does not declare
	BOOST_TEST(!drv->create_bind_group(group_of(one, { ubo_entry(0, uniform, 64), ubo_entry(1, uniform, 64) })));
	BOOST_TEST(state().count(vl::rule::bind_group_layout_mismatch) == 4u);

	// the entry names a storage buffer where the layout says uniform
	BOOST_TEST(!drv->create_bind_group(group_of(one, { ubo_entry(0, storage, 64) })));
	BOOST_TEST(state().count(vl::rule::bind_group_layout_mismatch) == 5u);

	// a texture slot that holds no texture, and a texture that cannot be sampled
	BOOST_TEST(!drv->create_bind_group(group_of(tex, { texture_entry(0, nullptr) })));
	BOOST_TEST(state().count(vl::rule::bind_group_layout_mismatch) == 6u);
	BOOST_TEST(!drv->create_bind_group(group_of(tex, { texture_entry(0, target_only.get()) })));
	BOOST_TEST(state().count(vl::rule::texture_not_sampled) == 1u);

	BOOST_TEST(state().total() == 7u);
	BOOST_TEST(inner().live_bind_groups() == 0u);

	BOOST_TEST(static_cast<bool>(drv->create_bind_group(group_of(tex, { texture_entry(0, sampled.get()) }))));
	BOOST_TEST(static_cast<bool>(drv->create_bind_group(group_of(one, { ubo_entry(0, uniform, 64) }))));
	BOOST_TEST(state().total() == 7u);
}

BOOST_AUTO_TEST_CASE(ABufferRangeOutsideTheBufferIsRejected)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));

	BOOST_TEST(!drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 65) })));
	BOOST_TEST(!drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 32, 48) })));
	BOOST_TEST(!drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 0) })));
	BOOST_TEST(state().count(vl::rule::invalid_bind_group) == 3u);
	BOOST_TEST(static_cast<bool>(drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 16, 48) }))));
	BOOST_TEST(state().total() == 3u);
}

BOOST_AUTO_TEST_CASE(AGroupOfAForeignLayoutIsRejected)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	bind_group_desc desc = group_of({ 99, 1 }, { ubo_entry(0, buffer, 64) });
	BOOST_TEST(!drv->create_bind_group(desc));
	expect_only(vl::rule::invalid_bind_group);
}

BOOST_AUTO_TEST_CASE(ATransientGroupUsedAfterItsFrameIsReported)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	drv->begin_frame();
	const auto transient = drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 64) }), lifetime::transient);
	const auto persistent = drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 64) }), lifetime::persistent);

	open_pass();
	drv->set_bind_group(frame_set, transient);
	drv->set_bind_group(frame_set, persistent);
	BOOST_TEST(state().total() == 0u);
	drv->end_render_pass();

	drv->begin_frame();
	open_pass();
	drv->set_bind_group(frame_set, persistent);
	BOOST_TEST(state().total() == 0u);
	drv->set_bind_group(frame_set, transient);
	expect_only(vl::rule::transient_group_expired);
	BOOST_TEST(inner().calls().bind_group_binds == 3u);	// two in the first frame, the persistent one in the second
}

BOOST_AUTO_TEST_CASE(ATransientGroupIsForgottenAfterFewFramesAndThenOnlyInvalid)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto transient = drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 64) }), lifetime::transient);
	for (int frame = 0; frame < 20; ++frame) {
		drv->begin_frame();
	}
	open_pass();
	drv->set_bind_group(frame_set, transient);
	expect_only(vl::rule::invalid_bind_group);
}

BOOST_AUTO_TEST_CASE(ADynamicOffsetThatIsNotAlignedIsReportedAndNotForwarded)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 2048, .update = buffer_update::per_frame });
	const auto group = drv->create_bind_group(group_of(vertex_layout, { ubo_entry(0, buffer, 80) }));
	open_pass();

	const std::uint32_t aligned[] = { 512 };
	const std::uint32_t zero[] = { 0 };
	drv->set_bind_group(object_set, group, aligned);
	drv->set_bind_group(object_set, group, zero);
	BOOST_TEST(state().total() == 0u);

	const std::uint32_t misaligned[] = { 100 };
	drv->set_bind_group(object_set, group, misaligned);
	expect_only(vl::rule::dynamic_offset_alignment);
	const std::uint32_t off_by_one[] = { 257 };
	drv->set_bind_group(object_set, group, off_by_one);
	expect_only(vl::rule::dynamic_offset_alignment, 2);
	BOOST_TEST(inner().calls().bind_group_binds == 2u);
}

BOOST_AUTO_TEST_CASE(AStaticOffsetOfABufferRangeHasToBeAlignedToo)
{
	const buffer_handle uniform_buffer = drv->create_buffer({ .size = 4096, .usage = buffer_usage::uniform });
	const buffer_handle storage_buffer = drv->create_buffer({ .size = 4096, .usage = buffer_usage::storage });
	const auto uniform_layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0, shader_stage::vertex, false } }));
	const auto storage_layout = drv->create_bind_group_layout(layout_of({ { binding_type::ssbo, 0, shader_stage::vertex, false } }));
	BOOST_TEST(drv->storage_offset_alignment() == 256u);
	open_pass();

	// Aligned offsets are fine; a group made at an unaligned one is reported when it is bound and not forwarded.
	const auto uniform_ok = drv->create_bind_group(group_of(uniform_layout, { ubo_entry(0, uniform_buffer, 64, 512) }));
	const auto storage_ok = drv->create_bind_group(group_of(storage_layout, { ssbo_entry_at(0, storage_buffer, 64, 256) }));
	drv->set_bind_group(frame_set, uniform_ok);
	drv->set_bind_group(object_set, storage_ok);
	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(inner().calls().bind_group_binds == 2u);

	const auto uniform_bad = drv->create_bind_group(group_of(uniform_layout, { ubo_entry(0, uniform_buffer, 64, 16) }));
	drv->set_bind_group(frame_set, uniform_bad);
	expect_only(vl::rule::binding_offset_alignment);
	const auto storage_bad = drv->create_bind_group(group_of(storage_layout, { ssbo_entry_at(0, storage_buffer, 64, 128) }));
	drv->set_bind_group(object_set, storage_bad);
	expect_only(vl::rule::binding_offset_alignment, 2);
	BOOST_TEST(inner().calls().bind_group_binds == 2u);
	BOOST_TEST(std::string(vl::rule_name(vl::rule::binding_offset_alignment)) == "binding_offset_alignment");
}

BOOST_AUTO_TEST_CASE(TheAlignmentIsTheOneOfTheBackend)
{
	BOOST_TEST(drv->uniform_offset_alignment() == 256u);
	const buffer_handle buffer = drv->create_buffer({ .size = 4096 });
	const auto group = drv->create_bind_group(group_of(vertex_layout, { ubo_entry(0, buffer, 80) }));
	open_pass();
	const std::uint32_t quarter[] = { 64 };	// fine for a GPU with 64, not for the 256 the null backend reports
	drv->set_bind_group(object_set, group, quarter);
	expect_only(vl::rule::dynamic_offset_alignment);
}

BOOST_AUTO_TEST_CASE(DynamicOffsetsHaveToMatchTheDynamicBindingsAndStayInTheBuffer)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 1024 });
	const auto fixed_layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto fixed = drv->create_bind_group(group_of(fixed_layout, { ubo_entry(0, buffer, 64) }));
	const auto dynamic = drv->create_bind_group(group_of(vertex_layout, { ubo_entry(0, buffer, 80) }));
	open_pass();
	const std::uint32_t one[] = { 256 };

	drv->set_bind_group(frame_set, dynamic);	// the dynamic binding gets no offset
	expect_only(vl::rule::invalid_bind_group);
	drv->set_bind_group(frame_set, fixed, one);	// the fixed group gets one
	expect_only(vl::rule::invalid_bind_group, 2);

	const std::uint32_t past_the_end[] = { 1024 };	// aligned, but the range lies past the end of the buffer
	drv->set_bind_group(object_set, dynamic, past_the_end);
	expect_only(vl::rule::invalid_bind_group, 3);
	const std::uint32_t last[] = { 768 };	// 768 + 80 <= 1024
	drv->set_bind_group(object_set, dynamic, last);
	BOOST_TEST(state().total() == 3u);
	BOOST_TEST(inner().calls().bind_group_binds == 1u);
}

BOOST_AUTO_TEST_CASE(BindGroupsOutsideARenderPassAreReported)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto group = drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 64) }));
	drv->set_bind_group(frame_set, group);
	expect_only(vl::rule::bind_group_outside_pass);
	BOOST_TEST(inner().calls().bind_group_binds == 0u);
}

BOOST_AUTO_TEST_CASE(ADrawWhosePipelineNeedsAnUnboundSetIsReported)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 1024 });
	const auto frame_layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto frame = drv->create_bind_group(group_of(frame_layout, { ubo_entry(0, buffer, 64) }));
	const auto object = drv->create_bind_group(group_of(vertex_layout, { ubo_entry(0, buffer, 80) }));
	auto geom = make_geometry(*drv);
	const pipeline_handle pipeline = pipeline_with({ frame_layout, {}, {}, vertex_layout });
	const std::uint32_t offset[] = { 0 };

	open_pass();
	drv->bind_pipeline(pipeline);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	BOOST_TEST(state().count(vl::rule::draw_missing_bind_group) == 2u);	// set 0 and set 3

	drv->set_bind_group(frame_set, frame);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	BOOST_TEST(state().count(vl::rule::draw_missing_bind_group) == 3u);	// only set 3 is left

	drv->set_bind_group(object_set, object, offset);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	BOOST_TEST(state().count(vl::rule::draw_missing_bind_group) == 3u);
	BOOST_TEST(state().total() == 3u);
	drv->end_render_pass();

	// The next pass starts with nothing bound.
	open_pass();
	drv->bind_pipeline(pipeline);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	BOOST_TEST(state().count(vl::rule::draw_missing_bind_group) == 5u);
}

BOOST_AUTO_TEST_CASE(ASetWithoutALayoutInThePipelineNeedsNoGroup)
{
	auto geom = make_geometry(*drv);
	const pipeline_handle pipeline = pipeline_with({});
	open_pass();
	drv->bind_pipeline(pipeline);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(ADrawWithAGroupOfAnotherLayoutIsReported)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 1024 });
	const auto other = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 1 } }));
	const auto group = drv->create_bind_group(group_of(other, { ubo_entry(1, buffer, 64) }));
	auto geom = make_geometry(*drv);
	const pipeline_handle pipeline = pipeline_with({ vertex_layout });

	open_pass();
	drv->bind_pipeline(pipeline);
	drv->set_bind_group(frame_set, group);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	expect_only(vl::rule::bind_group_layout_mismatch);
}

BOOST_AUTO_TEST_CASE(ADrawAfterTheBoundGroupWasDestroyedIsReported)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 1024 });
	const auto group = drv->create_bind_group(group_of(vertex_layout, { ubo_entry(0, buffer, 80) }));
	auto geom = make_geometry(*drv);
	const pipeline_handle pipeline = pipeline_with({ vertex_layout });
	const std::uint32_t offset[] = { 0 };

	open_pass();
	drv->bind_pipeline(pipeline);
	drv->set_bind_group(frame_set, group, offset);
	drv->destroy_bind_group(group);
	drv->draw_indices(geom.array.get(), 3, 0, 0);
	expect_only(vl::rule::invalid_bind_group);
}

BOOST_AUTO_TEST_CASE(APipelineOfAForeignLayoutIsRejected)
{
	program_object = drv->create_shader(program());
	pipeline_desc desc;
	desc.shader = program_object.get();
	desc.set_layouts.push_back({ 77, 1 });
	BOOST_TEST(!drv->create_pipeline(desc));
	expect_only(vl::rule::invalid_pipeline);
}

BOOST_AUTO_TEST_CASE(PipelinesWithAnotherLayoutAtASetAreAnotherPipeline)
{
	const auto other = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 1 } }));
	const pipeline_handle a = pipeline_with({ vertex_layout });
	pipeline_desc desc = drv->pipeline_info(a);
	const pipeline_handle same = drv->create_pipeline(desc);
	desc.set_layouts[0] = other;
	const pipeline_handle changed = drv->create_pipeline(desc);
	BOOST_TEST((a == same));
	BOOST_TEST((a != changed));
}

BOOST_AUTO_TEST_CASE(BuffersAreValidated)
{
	BOOST_TEST(!drv->create_buffer({ .size = 0 }));
	expect_only(vl::rule::invalid_buffer);

	const buffer_handle buffer = drv->create_buffer({ .size = 16 });
	drv->destroy_buffer(buffer);
	drv->destroy_buffer(buffer);
	BOOST_TEST(state().count(vl::rule::invalid_buffer) == 2u);
	drv->update_buffer(buffer, 0, bytes_of(1));
	BOOST_TEST(state().count(vl::rule::invalid_buffer) == 3u);
	BOOST_TEST(inner().calls().buffer_updates == 0u);
}

BOOST_AUTO_TEST_CASE(AGroupWhoseBufferWasDestroyedCannotBeBound)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 64 });
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const auto group = drv->create_bind_group(group_of(layout, { ubo_entry(0, buffer, 64) }));
	drv->destroy_buffer(buffer);
	open_pass();
	drv->set_bind_group(frame_set, group);
	expect_only(vl::rule::invalid_buffer);
	BOOST_TEST(inner().calls().bind_group_binds == 0u);
}

BOOST_AUTO_TEST_CASE(ATextureDeletedWhileAGroupHoldsItIsReportedAtBind)
{
	const auto layout = drv->create_bind_group_layout(layout_of({ { binding_type::texture, 0 } }));
	auto texture = drv->create_texture(texture_of(texture_usage::sampled));
	const auto group = drv->create_bind_group(group_of(layout, { texture_entry(0, texture.get()) }));
	texture.reset();
	open_pass();
	drv->set_bind_group(pass_set, group);
	expect_only(vl::rule::dead_object);
	BOOST_TEST(inner().calls().bind_group_binds == 0u);
}

BOOST_AUTO_TEST_CASE(PersistentGroupsAreCountedAgainstTheBudgetTransientOnesAreNot)
{
	bind_fixture limited(vl::limits{ .max_bind_groups = 2 });
	auto& d = *limited.drv;
	const buffer_handle buffer = d.create_buffer({ .size = 64 });
	const auto layout = d.create_bind_group_layout(layout_of({ { binding_type::ubo, 0 } }));
	const bind_group_desc desc = group_of(layout, { ubo_entry(0, buffer, 64) });

	for (int i = 0; i < 10; ++i) {
		d.create_bind_group(desc, lifetime::transient);
	}
	d.create_bind_group(desc);
	d.create_bind_group(desc);
	BOOST_TEST(limited.state().total() == 0u);
	BOOST_TEST(limited.state().current_frame().bind_groups == 2u);
	BOOST_TEST(limited.state().current_frame().transient_bind_groups == 10u);

	d.create_bind_group(desc);
	BOOST_TEST(limited.state().count(vl::rule::budget_bind_groups) == 1u);
	d.create_bind_group(desc);
	BOOST_TEST(limited.state().count(vl::rule::budget_bind_groups) == 1u);	// reported once per frame

	limited.chain.present();
	d.create_bind_group(desc);
	BOOST_TEST(limited.state().count(vl::rule::budget_bind_groups) == 1u);
}

BOOST_AUTO_TEST_CASE(TheDecoratorForwardsFrameAndBufferCalls)
{
	const buffer_handle buffer = drv->create_buffer({ .size = 8, .update = buffer_update::per_frame });
	drv->update_buffer(buffer, 4, bytes_of(9));
	drv->begin_frame();
	BOOST_TEST(inner().frame() == 1u);
	BOOST_TEST(inner().calls().frames_begun == 1u);
	BOOST_TEST(read_int(inner().buffer_bytes(buffer, 0), 4) == 9);
	drv->destroy_buffer(buffer);
	BOOST_TEST(inner().live_buffers() == 0u);
}

BOOST_AUTO_TEST_SUITE_END()

// --- the shader preprocessor ---

BOOST_AUTO_TEST_SUITE(RndShaderBindingMacroTests)

namespace {
	std::string expanded(std::string source, std::string* errors = nullptr)
	{
		std::string problems = rnd::expand_binding_macros(source, per_type_binding_table);
		if (errors) {
			*errors = problems;
		}
		return source;
	}
}

BOOST_AUTO_TEST_CASE(MacrosBecomeLayoutQualifiersWithTheBackendsBindings)
{
	std::string errors;
	BOOST_TEST(expanded("SNK_UBO(0, 1) uniform GlobalLights {};", &errors) == "layout(std140, binding = 1) uniform GlobalLights {};");
	BOOST_TEST(errors.empty());
	BOOST_TEST(expanded("SNK_UBO(3, 0) uniform ObjectData {};") == "layout(std140, binding = 6) uniform ObjectData {};");
	BOOST_TEST(expanded("SNK_SSBO(3, 1) readonly buffer IndexesBuffer {};") == "layout(std430, binding = 1) readonly buffer IndexesBuffer {};");
	BOOST_TEST(expanded("SNK_TEX(2, 3) uniform sampler2D t;") == "layout(binding = 7) uniform sampler2D t;");
}

BOOST_AUTO_TEST_CASE(SeveralMacrosAndSpacingAreHandled)
{
	const std::string out = expanded("SNK_UBO( 0 ,0 ) uniform A {};\nvoid f();\nSNK_TEX(1,1) uniform sampler2D b;\n");
	BOOST_TEST(out == "layout(std140, binding = 0) uniform A {};\nvoid f();\nlayout(binding = 1) uniform sampler2D b;\n");
}

BOOST_AUTO_TEST_CASE(MacrosInLineCommentsAndOtherIdentifiersAreLeftAlone)
{
	const std::string source = "// SNK_UBO(0, 0) is how a block is declared\nint SNK_UBO_COUNT;\n#define SNK_VERTEX_SHADER\nint MY_SNK_TEX(1);\n";
	std::string errors;
	BOOST_TEST(expanded(source, &errors) == source);
	BOOST_TEST(errors.empty());
}

BOOST_AUTO_TEST_CASE(AMacroOutsideTheTableIsReportedAndLeftInTheSource)
{
	std::string errors;
	BOOST_TEST(expanded("SNK_UBO(0, 2) uniform A {};", &errors) == "SNK_UBO(0, 2) uniform A {};");
	BOOST_TEST(errors.find("SNK_UBO(0, 2)") != std::string::npos);
	expanded("SNK_SSBO(0, 0) buffer B {};", &errors);
	BOOST_TEST(!errors.empty());
	expanded("SNK_TEX(4, 0) uniform sampler2D c;", &errors);
	BOOST_TEST(!errors.empty());
}

BOOST_AUTO_TEST_CASE(MacrosInBlockCommentsAreLeftAlone)
{
	std::string errors;
	const std::string one_line = "/* SNK_UBO(0, 0) uniform A {}; */ int x;\n";
	BOOST_TEST(expanded(one_line, &errors) == one_line);
	BOOST_TEST(errors.empty());

	const std::string spanning = "int a;\n/*\n   SNK_SSBO(3, 0) buffer B {};\n   // not a line comment inside, SNK_TEX(9, 9)\n*/\nint b;\n";
	BOOST_TEST(expanded(spanning, &errors) == spanning);
	BOOST_TEST(errors.empty());

	// The comment ends where it says: a macro behind it, on the same line or the next, is expanded.
	BOOST_TEST(expanded("/* SNK_UBO(0, 0) */ SNK_UBO(0, 1) uniform A {};") == "/* SNK_UBO(0, 0) */ layout(std140, binding = 1) uniform A {};");
	BOOST_TEST(expanded("/* a */\nSNK_TEX(1, 0) uniform sampler2D t;") == "/* a */\nlayout(binding = 0) uniform sampler2D t;");

	// Neither comment form hides the other.
	BOOST_TEST(expanded("// /* SNK_UBO(0, 1) uniform A {};\n") == "// /* SNK_UBO(0, 1) uniform A {};\n");
	BOOST_TEST(expanded("// start /*\nSNK_UBO(0, 1) uniform A {};\n") == "// start /*\nlayout(std140, binding = 1) uniform A {};\n");
	BOOST_TEST(expanded("/* // */ SNK_UBO(0, 1) uniform A {};") == "/* // */ layout(std140, binding = 1) uniform A {};");
	// A line comment goes on over a backslash at the end of the line.
	BOOST_TEST(expanded("// continued \\\nSNK_UBO(0, 1) uniform A {};\n") == "// continued \\\nSNK_UBO(0, 1) uniform A {};\n");
	BOOST_TEST(errors.empty());
}

BOOST_AUTO_TEST_CASE(ABadMacroInsideACommentCannotFailAShader)
{
	std::string errors;
	expanded("/* SNK_UBO(7, 7) and SNK_FOO(1) and SNK_TEX */\n// SNK_BAR(2)\n", &errors);
	BOOST_TEST(errors.empty());
}

BOOST_AUTO_TEST_CASE(AnotherNameCalledLikeAMacroIsReportedWithItsLine)
{
	std::string errors;
	const std::string source = "int a;\nSNK_UBOO(0, 0) uniform A {};\nSNK_BUFFER (1, 1) buffer B {};\n";
	BOOST_TEST(expanded(source, &errors) == source);
	BOOST_TEST(errors.find("line 2: SNK_UBOO") != std::string::npos);
	BOOST_TEST(errors.find("line 3: SNK_BUFFER") != std::string::npos);

	// The engine's own defines and names that are not calls are not macros.
	expanded("#ifdef SNK_VERTEX_SHADER\n#endif\n#ifdef SNK_FRAGMENT_SHADER\n#endif\nint SNK_value = 1;\n", &errors);
	BOOST_TEST(errors.empty());
}

BOOST_AUTO_TEST_CASE(EveryProblemIsReportedWithItsLine)
{
	std::string errors;
	expanded("SNK_UBO(0, 9) uniform A {};\n\nSNK_TEX 0 uniform sampler2D c;\nSNK_SSBO(0, 0) buffer B {};\n", &errors);
	BOOST_TEST(errors.find("line 1: SNK_UBO(0, 9) is outside the binding table") != std::string::npos);
	BOOST_TEST(errors.find("line 3: SNK_TEX needs (set, slot)") != std::string::npos);
	BOOST_TEST(errors.find("line 4: SNK_SSBO(0, 0) is outside the binding table") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(DefinesGoAfterTheVersionLineWhereverItIs)
{
	const auto inserted = [](std::string source, bool* ok = nullptr) {
		const bool result = rnd::insert_after_version(source, "#define A\n");
		if (ok) {
			*ok = result;
		}
		return source;
	};
	bool ok = false;
	BOOST_TEST(inserted("#version 450 core\nvoid main() {}\n", &ok) == "#version 450 core\n#define A\nvoid main() {}\n");
	BOOST_TEST(ok);
	// Leading comments and blank lines do not move it, and a #version in a comment is not the directive.
	BOOST_TEST(inserted("// licence\n\n/* more\n#version 100\n*/\n  #  version 450 core\nint x;\n", &ok)
		== "// licence\n\n/* more\n#version 100\n*/\n  #  version 450 core\n#define A\nint x;\n");
	BOOST_TEST(ok);
	BOOST_TEST(inserted("#version 450 core\r\nint x;\r\n", &ok) == "#version 450 core\r\n#define A\nint x;\r\n");
	BOOST_TEST(ok);
	BOOST_TEST(inserted("#version 450 core", &ok) == "#version 450 core\n#define A\n");
	BOOST_TEST(ok);

	// A byte order mark in front of the directive is dropped, not mistaken for code.
	BOOST_TEST(inserted("\xEF\xBB\xBF#version 450 core\nint x;\n", &ok) == "#version 450 core\n#define A\nint x;\n");
	BOOST_TEST(ok);
	BOOST_TEST(inserted("\xEF\xBB\xBF// licence\n#version 450 core\n", &ok) == "// licence\n#version 450 core\n#define A\n");
	BOOST_TEST(ok);

	// Anything else before it: the shader is not valid, the source stays as it was.
	for (const char* source : { "int x;\n#version 450 core\n", "#define B\n#version 450 core\n", "void main() {}\n", "", "// #version 450 core\n", "#versions 450\n" }) {
		BOOST_TEST_CONTEXT(source) {
			BOOST_TEST(inserted(source, &ok) == source);
			BOOST_TEST(!ok);
		}
	}
}

BOOST_AUTO_TEST_CASE(AMalformedMacroIsReported)
{
	std::string errors;
	expanded("SNK_UBO(set, slot) uniform A {};", &errors);
	BOOST_TEST(!errors.empty());
	expanded("SNK_TEX 0 uniform sampler2D c;", &errors);
	BOOST_TEST(!errors.empty());
	expanded("SNK_UBO(0) uniform A {};", &errors);
	BOOST_TEST(!errors.empty());
}

BOOST_AUTO_TEST_SUITE_END()
