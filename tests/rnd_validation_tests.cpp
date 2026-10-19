#include <boost/test/unit_test.hpp>
#include "rnd_validation_driver.h"
#include "rnd_null_driver.h"
#include "rnd_null_swapchain.h"
#include <memory>
#include <span>
#include <vector>

// Contract of the validation decorator over the null backend: each rule gets a deliberately wrong call
// sequence that must produce exactly the expected violation, and a correct sequence that produces none.

namespace {

using namespace rnd::driver;
namespace vl = rnd::driver::validation;

texture_header texture_of(uint32_t width, uint32_t height, texture_usage usage)
{
	texture_header header;
	header.data.extent.width = width;
	header.data.extent.height = height;
	header.usage = usage;
	return header;
}

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

// A triangle list: `indices` 32-bit indices in the index buffer, a vertex buffer with or without a layout.
struct geometry
{
	std::unique_ptr<vertex_array_interface> array;
	std::shared_ptr<buffer_interface> vertices;
	std::shared_ptr<buffer_interface> indices;
};

geometry make_geometry(driver_interface& drv, std::size_t index_count, bool with_layout = true)
{
	geometry result;
	result.array = drv.create_vertex_array();
	result.vertices = drv.create_buffer();
	if (with_layout) {
		result.vertices->set_layout({ { SHADER_DATA_TYPE::VEC3_F, "position" } });
	}
	result.vertices->set_data(std::vector<float>(9));
	result.array->add_vertex_buffer(result.vertices);
	result.indices = drv.create_buffer();
	result.indices->set_data(std::vector<unsigned int>(index_count));
	result.array->set_index_buffer(result.indices);
	return result;
}

render_pass_desc pass_of(texture_interface* color, texture_interface* depth = nullptr, const char* name = "test")
{
	render_pass_desc pass;
	if (color) {
		pass.colors.push_back({ .texture = color });
	}
	if (depth) {
		pass.depth_stencil = attachment_desc{ .texture = depth };
	}
	pass.debug_name = name;
	return pass;
}

struct validation_fixture
{
	validation_fixture()
		: chain(std::make_unique<null::swapchain>())
		, drv(chain.create_driver())
	{
	}

	vl::validator& state() { return chain.state(); }
	null::driver& inner() { return dynamic_cast<null::driver&>(dynamic_cast<vl::driver&>(*drv).inner()); }

	// A render pass on a small color target, unless one is already open: draws are only valid inside a pass.
	void open_pass()
	{
		if (!state().in_pass()) {
			pass_target = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
			drv->begin_render_pass(pass_of(pass_target.get()));
		}
	}

	// A pipeline for the pass in progress: same formats as its targets.
	pipeline_desc pipeline_for_pass()
	{
		program_object = drv->create_shader(program());
		pipeline_desc desc;
		desc.shader = program_object.get();
		desc.color_formats = state().current_formats().colors;
		desc.depth_format = state().current_formats().depth;
		return desc;
	}

	// Passes the whole prologue of a draw: a pipeline bound inside a render pass.
	void bind_test_pipeline()
	{
		open_pass();
		drv->bind_pipeline(drv->create_pipeline(pipeline_for_pass()));
	}

	vl::swapchain chain;
	std::unique_ptr<driver_interface> drv;
	std::unique_ptr<shader_interface> program_object;
	std::unique_ptr<texture_interface> pass_target;
};

void expect_only(const vl::validator& state, vl::rule kind, std::size_t count = 1)
{
	BOOST_TEST(state.total() == count);
	BOOST_TEST(state.count(kind) == count);
	if (!state.violations().empty()) {
		BOOST_TEST(static_cast<int>(state.violations().back().kind) == static_cast<int>(kind));
	}
}

}

BOOST_FIXTURE_TEST_SUITE(RndValidationTests, validation_fixture)

BOOST_AUTO_TEST_CASE(ACorrectFrameProducesNoViolations)
{
	auto color = drv->create_texture(texture_of(64, 32, texture_usage::color_target));
	auto depth = drv->create_texture(texture_of(64, 32, texture_usage::depth_target));
	auto mesh = make_geometry(*drv, 6);
	const buffer_handle ubo = drv->create_buffer({ .size = 64, .usage = buffer_usage::uniform });
	const buffer_handle ssbo = drv->create_buffer({ .size = 128, .usage = buffer_usage::storage });
	std::vector<std::byte> bytes(64);

	for (int frame = 0; frame < 3; ++frame) {
		render_pass_desc pass = pass_of(color.get(), depth.get());
		pass.colors[0].load = load_op::clear;
		pass.render_area = { 8, 8, 16, 16 };
		drv->begin_render_pass(pass);
		drv->set_viewport({ 0, 0, 64, 32 });
		drv->update_buffer(ubo, 0, bytes);
		drv->update_buffer(ssbo, 64, bytes);
		bind_test_pipeline();
		drv->draw_indices(mesh.array.get(), 6, 0, 0);
		drv->draw_indices(mesh.array, 3);
		drv->end_render_pass();
		drv->set_viewport({ 0, 0, 640, 480 });
		chain.present();
	}

	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(state().frame() == 3u);
	BOOST_TEST(state().last_frame().draws == 2u);
	BOOST_TEST(state().last_frame().frame_buffers == 1u);
	BOOST_TEST(inner().calls().draws == 6u);
	BOOST_TEST(inner().calls().render_pass_begins == 3u);
	BOOST_TEST(inner().calls().render_pass_ends == 3u);
}

BOOST_AUTO_TEST_CASE(PassesBalancedByEndsLeaveNothingOpenAtPresent)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));
	drv->end_render_pass();
	drv->begin_render_pass(pass_of(color.get()));
	drv->end_render_pass();
	chain.present();

	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(NullAttachmentsAreSkippedByTheChecks)
{
	auto depth = drv->create_texture(texture_of(64, 64, texture_usage::depth_target));
	drv->begin_render_pass(pass_of(nullptr, depth.get()));
	drv->end_render_pass();
	render_pass_desc holes = pass_of(nullptr, depth.get());
	holes.colors.push_back({});
	holes.colors.push_back({});
	drv->begin_render_pass(holes);
	drv->end_render_pass();

	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(ViewportOutsideTheRenderTargetIsReported)
{
	auto color = drv->create_texture(texture_of(64, 32, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));

	drv->set_viewport({ 0, 0, 64, 32 });
	drv->set_viewport({ 16, 8, 48, 24 });
	BOOST_TEST(state().total() == 0u);

	drv->set_viewport({ 0, 0, 65, 32 });
	expect_only(state(), vl::rule::viewport_outside_target, 1);
	drv->set_viewport({ 0, 0, 64, 33 });
	expect_only(state(), vl::rule::viewport_outside_target, 2);
	drv->set_viewport({ -1, 0, 8, 8 });
	expect_only(state(), vl::rule::viewport_outside_target, 3);
	BOOST_TEST((inner().viewport() == glm::ivec4(-1, 0, 8, 8)));
}

BOOST_AUTO_TEST_CASE(DrawBeforeAnyPipelineIsBoundIsReported)
{
	auto mesh = make_geometry(*drv, 3);
	open_pass();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);

	expect_only(state(), vl::rule::draw_without_pipeline);
	BOOST_TEST(inner().calls().draws == 1u);
}

BOOST_AUTO_TEST_CASE(ABoundPipelineCoversTheDrawsOfThePassAndRestartsAtTheNextPass)
{
	auto mesh = make_geometry(*drv, 3);
	bind_test_pipeline();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);
	BOOST_TEST(state().total() == 0u);

	drv->end_render_pass();
	chain.present();
	open_pass();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);
	expect_only(state(), vl::rule::draw_without_pipeline);
}

BOOST_AUTO_TEST_CASE(APipelineBoundInOnePassDoesNotCoverTheNextPassOfTheSameFrame)
{
	auto mesh = make_geometry(*drv, 3);
	bind_test_pipeline();
	drv->end_render_pass();
	open_pass();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);

	expect_only(state(), vl::rule::draw_without_pipeline);
}

BOOST_AUTO_TEST_CASE(DrawBeyondTheIndexBufferIsReported)
{
	auto mesh = make_geometry(*drv, 6);
	bind_test_pipeline();

	drv->draw_indices(mesh.array.get(), 6, 0, 0);
	drv->draw_indices(mesh.array.get(), 3, 0, 3);
	drv->draw_indices(mesh.array, 6);
	BOOST_TEST(state().total() == 0u);

	drv->draw_indices(mesh.array.get(), 7, 0, 0);
	expect_only(state(), vl::rule::draw_index_range, 1);
	drv->draw_indices(mesh.array.get(), 6, 0, 1);
	expect_only(state(), vl::rule::draw_index_range, 2);
	drv->draw_indices(mesh.array, 9);
	expect_only(state(), vl::rule::draw_index_range, 3);
}

BOOST_AUTO_TEST_CASE(DrawWithoutAnIndexBufferIsReported)
{
	geometry mesh;
	mesh.array = drv->create_vertex_array();
	mesh.vertices = drv->create_buffer();
	mesh.vertices->set_layout({ { SHADER_DATA_TYPE::VEC3_F, "position" } });
	mesh.vertices->set_data(std::vector<float>(9));
	mesh.array->add_vertex_buffer(mesh.vertices);
	bind_test_pipeline();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);

	expect_only(state(), vl::rule::draw_index_range);
}

BOOST_AUTO_TEST_CASE(IndexBufferSizeIsFixedByItsFirstAllocation)
{
	auto mesh = make_geometry(*drv, 3);
	mesh.indices->set_data(std::vector<unsigned int>(2));
	bind_test_pipeline();

	drv->draw_indices(mesh.array.get(), 3, 0, 0);
	BOOST_TEST(state().total() == 0u);

	mesh.indices->reserve(12 * sizeof(unsigned int));
	drv->draw_indices(mesh.array.get(), 12, 0, 0);
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(DrawWithoutAVertexLayoutIsReported)
{
	auto mesh = make_geometry(*drv, 3, false);
	bind_test_pipeline();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);

	expect_only(state(), vl::rule::draw_without_layout);
}

BOOST_AUTO_TEST_CASE(ALayoutSetAfterTheBufferWasAttachedDoesNotCount)
{
	auto mesh = make_geometry(*drv, 3, false);
	mesh.vertices->set_layout({ { SHADER_DATA_TYPE::VEC3_F, "position" } });
	bind_test_pipeline();
	drv->draw_indices(mesh.array.get(), 3, 0, 0);

	expect_only(state(), vl::rule::draw_without_layout);
}

BOOST_AUTO_TEST_CASE(BufferWritePastItsSizeIsReportedAndNotForwarded)
{
	const buffer_handle ubo = drv->create_buffer({ .size = 32, .usage = buffer_usage::uniform });
	std::vector<std::byte> bytes(64, std::byte{ 1 });
	const std::span<const std::byte> all{ bytes };

	drv->update_buffer(ubo, 0, all.first(32));
	drv->update_buffer(ubo, 16, all.first(16));
	drv->update_buffer(ubo, 32, all.first(0));
	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(inner().calls().buffer_updates == 3u);

	drv->update_buffer(ubo, 0, all.first(33));
	expect_only(state(), vl::rule::buffer_write_range, 1);
	drv->update_buffer(ubo, 24, all.first(16));
	expect_only(state(), vl::rule::buffer_write_range, 2);
	drv->update_buffer(ubo, 33, all.first(1));
	expect_only(state(), vl::rule::buffer_write_range, 3);
	BOOST_TEST(inner().calls().buffer_updates == 3u);
}

BOOST_AUTO_TEST_CASE(TextureUsedAsATargetAfterItWasDeletedIsReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	texture_interface* stale = color.get();
	BOOST_TEST(state().live_objects(vl::object_kind::texture) == 1u);
	color.reset();
	BOOST_TEST(state().live_objects(vl::object_kind::texture) == 0u);

	drv->begin_render_pass(pass_of(stale));

	expect_only(state(), vl::rule::dead_object);
	BOOST_TEST(inner().calls().render_pass_begins == 0u);
}

BOOST_AUTO_TEST_CASE(DepthAttachmentDeletedBeforeUseIsReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto depth = drv->create_texture(texture_of(16, 16, texture_usage::depth_target));
	texture_interface* stale = depth.get();
	depth.reset();
	drv->begin_render_pass(pass_of(color.get(), stale));

	expect_only(state(), vl::rule::dead_object);
}

BOOST_AUTO_TEST_CASE(VertexArrayDeletedBeforeADrawIsReportedAndNotDrawn)
{
	auto mesh = make_geometry(*drv, 3);
	bind_test_pipeline();
	const vertex_array_interface* stale = mesh.array.get();
	mesh.array.reset();
	drv->draw_indices(stale, 3, 0, 0);

	expect_only(state(), vl::rule::dead_object);
	BOOST_TEST(inner().calls().draws == 0u);
}

BOOST_AUTO_TEST_CASE(ADrawOfNullIsReportedAsADeadObject)
{
	bind_test_pipeline();
	drv->draw_indices(static_cast<const vertex_array_interface*>(nullptr), 3, 0, 0);

	expect_only(state(), vl::rule::dead_object);
}

BOOST_AUTO_TEST_CASE(ObjectsOfEveryKindAreRegisteredWhileTheyLive)
{
	{
		auto shader = drv->create_shader(program());
		auto texture = drv->create_texture(texture_of(4, 4, texture_usage::sampled));
		auto array = drv->create_vertex_array();
		auto buffer = drv->create_buffer();
		BOOST_TEST(state().live_objects(vl::object_kind::shader) == 1u);
		BOOST_TEST(state().live_objects(vl::object_kind::texture) == 1u);
		BOOST_TEST(state().live_objects(vl::object_kind::vertex_array) == 1u);
		BOOST_TEST(state().live_objects(vl::object_kind::buffer) == 1u);
	}
	BOOST_TEST(inner().stats().live_total() == 0u);
	for (auto kind : { vl::object_kind::shader, vl::object_kind::texture, vl::object_kind::vertex_array,
		vl::object_kind::buffer }) {
		BOOST_TEST(state().live_objects(kind) == 0u);
	}
}

BOOST_AUTO_TEST_CASE(ARejectedShaderIsNotWrappedOrCounted)
{
	shader_header bad;
	bad.title = "bad.vert";
	bad.body = "void main() {}";
	bad.type = shader_header::shader_type::VERTEX;

	BOOST_TEST(!drv->create_shader({ bad }));
	BOOST_TEST(state().current_frame().programs == 0u);
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(FrameBufferBudgetIsReportedOncePerFrameAndRestartsAtPresent)
{
	vl::swapchain limited(std::make_unique<null::swapchain>(), vl::limits{ .max_frame_buffers = 2 });
	auto limited_drv = limited.create_driver();

	auto color = limited_drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	const auto run_pass = [&] {
		limited_drv->begin_render_pass(pass_of(color.get()));
		limited_drv->end_render_pass();
	};

	for (int i = 0; i < 4; ++i) {
		run_pass();
	}
	expect_only(limited.state(), vl::rule::budget_frame_buffers);

	limited.present();
	run_pass();
	run_pass();
	limited.present();
	expect_only(limited.state(), vl::rule::budget_frame_buffers);

	for (int i = 0; i < 3; ++i) {
		run_pass();
	}
	limited.present();
	expect_only(limited.state(), vl::rule::budget_frame_buffers, 2);
}

BOOST_AUTO_TEST_CASE(ProgramBudgetCountsProgramsCreatedInAFrame)
{
	vl::swapchain limited(std::make_unique<null::swapchain>(), vl::limits{ .max_programs = 2 });
	auto limited_drv = limited.create_driver();

	auto first = limited_drv->create_shader(program());
	auto second = limited_drv->create_shader(program());
	BOOST_TEST(limited.state().total() == 0u);
	auto third = limited_drv->create_shader(program());
	auto fourth = limited_drv->create_shader(program());

	expect_only(limited.state(), vl::rule::budget_programs);
	BOOST_TEST(limited.state().current_frame().programs == 4u);

	limited.present();
	BOOST_TEST(limited.state().last_frame().programs == 4u);
	BOOST_TEST(limited.state().current_frame().programs == 0u);
}

BOOST_AUTO_TEST_CASE(TextureBudgetCountsTexturesCreatedInAFrame)
{
	vl::swapchain limited(std::make_unique<null::swapchain>(), vl::limits{ .max_textures = 1 });
	auto limited_drv = limited.create_driver();

	auto first = limited_drv->create_texture(texture_of(4, 4, texture_usage::sampled));
	BOOST_TEST(limited.state().total() == 0u);
	auto second = limited_drv->create_texture(texture_of(4, 4, texture_usage::sampled));

	expect_only(limited.state(), vl::rule::budget_textures);
}

BOOST_AUTO_TEST_CASE(NoBudgetIsEnforcedByDefault)
{
	std::vector<std::unique_ptr<texture_interface>> textures;
	for (int i = 0; i < 100; ++i) {
		textures.push_back(drv->create_texture(texture_of(4, 4, texture_usage::sampled | texture_usage::color_target)));
		drv->begin_render_pass(pass_of(textures.back().get()));
		drv->end_render_pass();
	}
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(ACorrectRenderPassFrameProducesNoViolations)
{
	auto color = drv->create_texture(texture_of(64, 32, texture_usage::color_target));
	auto depth = drv->create_texture(texture_of(64, 32, texture_usage::depth_target));
	auto mesh = make_geometry(*drv, 6);

	for (int frame = 0; frame < 3; ++frame) {
		render_pass_desc pass = pass_of(color.get(), depth.get());
		pass.colors[0].load = load_op::clear;
		pass.depth_stencil->load = load_op::clear;
		pass.render_area = { 8, 8, 16, 16 };
		drv->begin_render_pass(pass);
		drv->set_viewport({ 0, 0, 64, 32 });
		bind_test_pipeline();
		drv->draw_indices(mesh.array.get(), 6, 0, 0);
		drv->end_render_pass();

		render_pass_desc window;
		window.backbuffer = true;
		window.colors.push_back({ .load = load_op::clear });
		window.depth_stencil = attachment_desc{ .load = load_op::clear };
		drv->begin_render_pass(window);
		drv->set_viewport({ 0, 0, 640, 480 });
		drv->end_render_pass();
		chain.present();
	}

	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(state().last_frame().frame_buffers == 2u);
	BOOST_TEST(inner().calls().render_pass_begins == 6u);
	BOOST_TEST(inner().calls().render_pass_ends == 6u);
	BOOST_TEST(inner().stats().framebuffers.misses() == 1u);
	BOOST_TEST(inner().stats().framebuffers.hits() == 2u);
}

BOOST_AUTO_TEST_CASE(ANestedRenderPassIsReportedAndNotForwarded)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto other = drv->create_texture(texture_of(16, 16, texture_usage::color_target));

	drv->begin_render_pass(pass_of(color.get(), nullptr, "outer"));
	drv->begin_render_pass(pass_of(other.get(), nullptr, "inner"));
	expect_only(state(), vl::rule::nested_render_pass);
	BOOST_TEST(inner().calls().render_pass_begins == 1u);

	// The end of the refused pass is swallowed, the outer pass stays open until its own end.
	drv->end_render_pass();
	BOOST_TEST(inner().in_render_pass());
	drv->end_render_pass();
	BOOST_TEST(!inner().in_render_pass());
	chain.present();

	BOOST_TEST(state().total() == 1u);
	BOOST_TEST(inner().calls().render_pass_ends == 1u);
}

BOOST_AUTO_TEST_CASE(EndRenderPassWithoutBeginIsReportedAndNotForwarded)
{
	drv->end_render_pass();

	expect_only(state(), vl::rule::end_without_begin);
	BOOST_TEST(inner().calls().render_pass_ends == 0u);
}

BOOST_AUTO_TEST_CASE(ARenderPassStillOpenAtPresentIsReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));
	chain.present();

	expect_only(state(), vl::rule::unbalanced_at_present);
}

BOOST_AUTO_TEST_CASE(RenderPassColorAttachmentWithoutColorTargetUsageIsReported)
{
	auto sampled = drv->create_texture(texture_of(16, 16, texture_usage::sampled));
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));
	drv->end_render_pass();
	BOOST_TEST(state().total() == 0u);

	drv->begin_render_pass(pass_of(sampled.get()));
	expect_only(state(), vl::rule::attachment_usage);
	BOOST_TEST(inner().calls().render_pass_begins == 2u);
}

BOOST_AUTO_TEST_CASE(RenderPassDepthAttachmentNeedsDepthOrStencilUsage)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto not_depth = drv->create_texture(texture_of(16, 16, texture_usage::color_target | texture_usage::sampled));
	auto depth = drv->create_texture(texture_of(16, 16, texture_usage::depth_target));
	auto stencil = drv->create_texture(texture_of(16, 16, texture_usage::stencil_target));

	drv->begin_render_pass(pass_of(color.get(), depth.get()));
	drv->end_render_pass();
	drv->begin_render_pass(pass_of(color.get(), stencil.get()));
	drv->end_render_pass();
	BOOST_TEST(state().total() == 0u);

	drv->begin_render_pass(pass_of(color.get(), not_depth.get()));
	expect_only(state(), vl::rule::attachment_usage);
}

BOOST_AUTO_TEST_CASE(EveryColorOfARenderPassIsCheckedForUsage)
{
	auto first = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto second = drv->create_texture(texture_of(16, 16, texture_usage::sampled));
	render_pass_desc pass = pass_of(first.get());
	pass.colors.push_back({ .texture = second.get() });
	drv->begin_render_pass(pass);

	expect_only(state(), vl::rule::attachment_usage);
}

BOOST_AUTO_TEST_CASE(RenderPassAttachmentsOfDifferentSizesAreReportedOnce)
{
	auto color = drv->create_texture(texture_of(64, 64, texture_usage::color_target));
	auto second = drv->create_texture(texture_of(64, 32, texture_usage::color_target));
	auto depth = drv->create_texture(texture_of(32, 32, texture_usage::depth_target));
	render_pass_desc pass = pass_of(color.get(), depth.get());
	pass.colors.push_back({ .texture = second.get() });
	drv->begin_render_pass(pass);

	expect_only(state(), vl::rule::attachment_size_mismatch);
}

BOOST_AUTO_TEST_CASE(AttachmentSizesAreTakenAtTheirMipLevel)
{
	texture_header mipped = texture_of(64, 64, texture_usage::color_target);
	mipped.data.mip_levels = 3;
	auto color = drv->create_texture(mipped);
	auto small = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	render_pass_desc pass = pass_of(color.get());
	pass.colors[0].mip = 2;
	pass.colors.push_back({ .texture = small.get() });
	drv->begin_render_pass(pass);
	drv->end_render_pass();
	BOOST_TEST(state().total() == 0u);

	render_pass_desc past_last_mip = pass_of(color.get());
	past_last_mip.colors[0].mip = 3;
	drv->begin_render_pass(past_last_mip);
	expect_only(state(), vl::rule::invalid_render_pass);
}

BOOST_AUTO_TEST_CASE(RenderPassLayerOutsideTheTextureIsReported)
{
	texture_header array = texture_of(16, 16, texture_usage::color_target);
	array.type = TEXTURE_TYPE::TEXTURE_2D_ARRAY;
	array.data.extent.layers = 4;
	texture_header cube = texture_of(16, 16, texture_usage::color_target);
	cube.type = TEXTURE_TYPE::TEXTURE_CUBE_MAP;
	auto layers = drv->create_texture(array);
	auto faces = drv->create_texture(cube);

	for (const auto& [texture, last] : { std::pair{ layers.get(), 3u }, std::pair{ faces.get(), 5u } }) {
		render_pass_desc pass = pass_of(texture);
		pass.colors[0].layer = last;
		drv->begin_render_pass(pass);
		drv->end_render_pass();
	}
	BOOST_TEST(state().total() == 0u);

	drv->begin_render_pass(pass_of(layers.get()));
	drv->end_render_pass();
	drv->begin_render_pass(pass_of(faces.get()));
	drv->end_render_pass();
	BOOST_TEST(state().total() == 0u);

	render_pass_desc past_end = pass_of(layers.get());
	past_end.colors[0].layer = 4;
	drv->begin_render_pass(past_end);
	expect_only(state(), vl::rule::invalid_render_pass);
}

BOOST_AUTO_TEST_CASE(ViewportAfterTheEndOfAPassIsNotCheckedAgainstItsTarget)
{
	auto color = drv->create_texture(texture_of(64, 32, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));
	drv->end_render_pass();
	drv->set_viewport({ 0, 0, 4096, 4096 });

	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(RenderAreaOutsideTheTargetIsReported)
{
	auto color = drv->create_texture(texture_of(64, 32, texture_usage::color_target));
	render_pass_desc pass = pass_of(color.get());

	pass.render_area = { 4, 4, 8, 8 };
	drv->begin_render_pass(pass);
	drv->end_render_pass();
	pass.render_area = { 0, 0, 0, 0 };
	drv->begin_render_pass(pass);
	drv->end_render_pass();
	BOOST_TEST(state().total() == 0u);

	pass.render_area = { 60, 0, 8, 8 };
	drv->begin_render_pass(pass);
	expect_only(state(), vl::rule::render_area_outside_target);
}

BOOST_AUTO_TEST_CASE(MalformedRenderPassesAreReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));

	drv->begin_render_pass(render_pass_desc{});
	expect_only(state(), vl::rule::invalid_render_pass, 1);
	drv->end_render_pass();

	render_pass_desc window = pass_of(color.get());
	window.backbuffer = true;
	drv->begin_render_pass(window);
	expect_only(state(), vl::rule::invalid_render_pass, 2);
	drv->end_render_pass();

	render_pass_desc no_depth_texture = pass_of(color.get());
	no_depth_texture.depth_stencil = attachment_desc{};
	drv->begin_render_pass(no_depth_texture);
	expect_only(state(), vl::rule::invalid_render_pass, 3);
}

BOOST_AUTO_TEST_CASE(ADeletedAttachmentDropsTheRenderPass)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto depth = drv->create_texture(texture_of(16, 16, texture_usage::depth_target));
	texture_interface* stale = depth.get();
	depth.reset();

	drv->begin_render_pass(pass_of(color.get(), stale));
	drv->end_render_pass();
	chain.present();

	expect_only(state(), vl::rule::dead_object);
	BOOST_TEST(inner().calls().render_pass_begins == 0u);
	BOOST_TEST(inner().calls().render_pass_ends == 0u);
}

BOOST_AUTO_TEST_CASE(RenderPassTexturesReachTheWrappedDriverUnwrapped)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get(), nullptr, "forwarded"));
	drv->end_render_pass();

	const render_pass_desc& seen = inner().last_render_pass();
	BOOST_REQUIRE(seen.colors.size() == 1u);
	BOOST_TEST(seen.colors[0].texture == color->native());
	BOOST_TEST(std::string(seen.debug_name) == "forwarded");
}

BOOST_AUTO_TEST_CASE(FramebuffersAreReusedAcrossFramesAndEvictedWithTheirTexture)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	for (int frame = 0; frame < 5; ++frame) {
		drv->begin_render_pass(pass_of(color.get()));
		drv->end_render_pass();
		chain.present();
	}
	BOOST_TEST(inner().stats().framebuffers.misses() == 1u);
	BOOST_TEST(inner().stats().framebuffers.hits() == 4u);

	color.reset();
	BOOST_TEST(inner().stats().framebuffers.size() == 0u);
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(TraceRecordsPassMarkersAndDrawsInOrder)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto mesh = make_geometry(*drv, 6);
	drv->begin_render_pass(pass_of(color.get()));
	bind_test_pipeline();
	drv->draw_indices(mesh.array.get(), 6, 0, 0);
	drv->end_render_pass();

	std::vector<int> ops;
	std::uint32_t draw_count = 0;
	for (const auto& entry : state().trace()) {
		if (entry.op == vl::trace_op::create_object) {
			continue;
		}
		ops.push_back(static_cast<int>(entry.op));
		if (entry.op == vl::trace_op::draw) {
			draw_count = entry.a;
		}
	}
	const std::vector<int> expected{ static_cast<int>(vl::trace_op::begin_pass), static_cast<int>(vl::trace_op::bind_pipeline),
		static_cast<int>(vl::trace_op::draw), static_cast<int>(vl::trace_op::end_pass) };
	BOOST_TEST(ops == expected, boost::test_tools::per_element());
	BOOST_TEST(draw_count == 6u);
}

BOOST_AUTO_TEST_CASE(TraceKeepsOnlyTheMostRecentCommands)
{
	auto shared = std::make_shared<vl::validator>(vl::limits{}, 4);
	vl::driver small_trace(std::make_unique<null::driver>(), shared);

	for (int i = 0; i < 10; ++i) {
		small_trace.set_viewport({ 0, 0, i, i });
	}
	render_pass_desc window;
	window.backbuffer = true;
	small_trace.begin_render_pass(window);

	const auto trace = shared->trace();
	BOOST_TEST(trace.size() == 4u);
	BOOST_TEST(static_cast<int>(trace.back().op) == static_cast<int>(vl::trace_op::begin_pass));
	BOOST_TEST(static_cast<int>(trace.front().op) == static_cast<int>(vl::trace_op::set_viewport));
}

BOOST_AUTO_TEST_CASE(ViolationsKeepTheirMessageAndTheFrameTheyHappenedIn)
{
	chain.present();
	chain.present();
	drv->end_render_pass();

	BOOST_REQUIRE(state().violations().size() == 1u);
	BOOST_TEST(state().violations().front().frame == 2u);
	BOOST_TEST(!state().violations().front().message.empty());
	BOOST_TEST(std::string(vl::rule_name(state().violations().front().kind)) == "end_without_begin");
}

BOOST_AUTO_TEST_CASE(TheDecoratorForwardsTheRestOfTheInterface)
{
	auto texture = drv->create_texture(texture_of(8, 8, texture_usage::sampled));
	texture->set_filtering(texture_header::FILTERING::NEAREST, texture_header::FILTERING::NEAREST);
	texture->bind(3);
	texture->generate_mipmaps();
	auto shader = drv->create_shader(program());
	auto array = drv->create_vertex_array();
	drv->set_line_size(2.f);
	drv->set_viewport({ 1, 2, 3, 4 });
	pipeline_desc to_create;
	to_create.shader = shader.get();
	to_create.topology = RENDER_MODE::LINE;
	const pipeline_handle pipeline = drv->create_pipeline(to_create);
	drv->invalidate_state_cache();

	BOOST_TEST(texture->width() == 8u);
	BOOST_TEST((texture->get_usage() == texture_usage::sampled));
	BOOST_TEST(texture->native() != texture.get());
	BOOST_TEST(inner().line_size() == 2.f);
	BOOST_TEST((inner().viewport() == glm::ivec4(1, 2, 3, 4)));
	BOOST_TEST(inner().pipelines().size() == 1u);
	BOOST_TEST((inner().pipeline_info(pipeline).topology == RENDER_MODE::LINE));
	BOOST_TEST(inner().calls().state_cache_invalidations == 1u);
	BOOST_TEST(inner().stats().textures.created == 1u);
	BOOST_TEST(inner().stats().shaders.created == 1u);
	BOOST_TEST(!drv->supports_timer_queries());
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(ObjectsMayOutliveTheDriverAndTheSwapchain)
{
	std::unique_ptr<texture_interface> texture;
	std::unique_ptr<buffer_interface> buffer;
	{
		vl::swapchain short_lived(std::make_unique<null::swapchain>());
		auto short_drv = short_lived.create_driver();
		texture = short_drv->create_texture(texture_of(4, 4, texture_usage::sampled));
		buffer = short_drv->create_buffer();
	}
	buffer->set_data(std::vector<float>(3));
	texture.reset();
	buffer.reset();
}

BOOST_AUTO_TEST_CASE(RenderTargetThatIsAlsoSampledPassesEveryUsageRule)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target | texture_usage::sampled));
	color->bind(0);
	drv->begin_render_pass(pass_of(color.get()));
	drv->end_render_pass();
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(TextureWithoutAnyUsageIsReportedOnCreation)
{
	auto texture = drv->create_texture(texture_of(8, 8, texture_usage::none));
	BOOST_TEST(static_cast<bool>(texture));
	expect_only(state(), vl::rule::texture_usage_flags);
}

BOOST_AUTO_TEST_CASE(DepthFormatWithoutDepthOrStencilTargetIsReportedOnCreation)
{
	texture_header header = texture_of(8, 8, texture_usage::sampled);
	header.data.format = texture_header::TYPE::D32;
	drv->create_texture(header);
	expect_only(state(), vl::rule::texture_usage_flags);
}

BOOST_AUTO_TEST_CASE(DepthFormatAsColorTargetIsReportedOnCreation)
{
	texture_header header = texture_of(8, 8, texture_usage::depth_target | texture_usage::color_target);
	header.data.format = texture_header::TYPE::D24_S8;
	drv->create_texture(header);
	expect_only(state(), vl::rule::texture_usage_flags);
}

BOOST_AUTO_TEST_CASE(DepthTargetThatIsAlsoSampledIsAccepted)
{
	texture_header header = texture_of(8, 8, texture_usage::depth_target | texture_usage::sampled);
	header.data.format = texture_header::TYPE::D32;
	drv->create_texture(header);
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(InitialDataRequiresSampledUsage)
{
	const unsigned char pixels[4] = { 1, 2, 3, 4 };
	texture_header header = texture_of(1, 1, texture_usage::color_target);
	header.data.initial_data = pixels;
	drv->create_texture(header);
	expect_only(state(), vl::rule::texture_usage_flags);

	header.usage = texture_usage::color_target | texture_usage::sampled;
	drv->create_texture(header);
	BOOST_TEST(state().total() == 1u);
}

BOOST_AUTO_TEST_CASE(BindingATextureWithoutSampledUsageIsReported)
{
	auto color = drv->create_texture(texture_of(8, 8, texture_usage::color_target));
	color->bind(2);
	expect_only(state(), vl::rule::texture_not_sampled);
}

BOOST_AUTO_TEST_CASE(APipelineMatchingThePassFormatsProducesNoViolations)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	texture_header depth_header = texture_of(16, 16, texture_usage::depth_target);
	depth_header.data.format = texture_header::TYPE::D32;
	auto depth = drv->create_texture(depth_header);
	auto mesh = make_geometry(*drv, 3);

	drv->begin_render_pass(pass_of(color.get(), depth.get()));
	pipeline_desc desc;
	program_object = drv->create_shader(program());
	desc.shader = program_object.get();
	desc.color_formats = { texture_format::RGBA8 };
	desc.depth_format = texture_format::D32;
	drv->bind_pipeline(drv->create_pipeline(desc));
	drv->draw_indices(mesh.array.get(), 3, 0, 0);
	drv->end_render_pass();

	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(inner().calls().pipeline_binds == 1u);
}

BOOST_AUTO_TEST_CASE(PipelineWithAnotherColorFormatThanThePassIsReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));
	pipeline_desc desc = pipeline_for_pass();
	desc.color_formats = { texture_format::RGBA16F };
	drv->bind_pipeline(drv->create_pipeline(desc));

	expect_only(state(), vl::rule::pipeline_format_mismatch);
}

BOOST_AUTO_TEST_CASE(PipelineWithADepthFormatForAPassWithoutDepthIsReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	drv->begin_render_pass(pass_of(color.get()));
	pipeline_desc desc = pipeline_for_pass();
	desc.depth_format = texture_format::D32;
	drv->bind_pipeline(drv->create_pipeline(desc));

	expect_only(state(), vl::rule::pipeline_format_mismatch);
}

BOOST_AUTO_TEST_CASE(PipelineWithoutADepthFormatForAPassWithDepthIsReported)
{
	auto color = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto depth = drv->create_texture(texture_of(16, 16, texture_usage::depth_target));
	drv->begin_render_pass(pass_of(color.get(), depth.get()));
	pipeline_desc desc = pipeline_for_pass();
	desc.depth_format.reset();
	drv->bind_pipeline(drv->create_pipeline(desc));

	expect_only(state(), vl::rule::pipeline_format_mismatch);
}

BOOST_AUTO_TEST_CASE(PipelineWithFewerColorFormatsThanThePassTargetsIsReported)
{
	auto first = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	auto second = drv->create_texture(texture_of(16, 16, texture_usage::color_target));
	render_pass_desc pass = pass_of(first.get());
	pass.colors.push_back({ .texture = second.get() });
	drv->begin_render_pass(pass);
	pipeline_desc desc = pipeline_for_pass();
	BOOST_TEST(desc.color_formats.size() == 2u);
	desc.color_formats.pop_back();
	drv->bind_pipeline(drv->create_pipeline(desc));

	expect_only(state(), vl::rule::pipeline_format_mismatch);
}

BOOST_AUTO_TEST_CASE(PipelinesForTheBackbufferDeclareTheWindowFormats)
{
	render_pass_desc window;
	window.backbuffer = true;
	window.debug_name = "window";
	drv->begin_render_pass(window);
	program_object = drv->create_shader(program());

	pipeline_desc right;
	right.shader = program_object.get();
	set_target_formats(right, window);
	drv->bind_pipeline(drv->create_pipeline(right));
	BOOST_TEST(state().total() == 0u);
	BOOST_TEST(right.color_formats.size() == 1u);
	BOOST_TEST((right.depth_format == backbuffer_depth_format));

	pipeline_desc wrong = right;
	wrong.depth_format.reset();
	drv->bind_pipeline(drv->create_pipeline(wrong));
	expect_only(state(), vl::rule::pipeline_format_mismatch);
}

BOOST_AUTO_TEST_CASE(BindingAPipelineOutsideARenderPassIsReportedAndNotForwarded)
{
	program_object = drv->create_shader(program());
	pipeline_desc desc;
	desc.shader = program_object.get();
	drv->bind_pipeline(drv->create_pipeline(desc));

	expect_only(state(), vl::rule::bind_pipeline_outside_pass);
	BOOST_TEST(inner().calls().pipeline_binds == 0u);
}

BOOST_AUTO_TEST_CASE(BindingAHandleOfNoPipelineIsReportedAndNotForwarded)
{
	open_pass();
	drv->bind_pipeline({});
	expect_only(state(), vl::rule::invalid_pipeline, 1);
	drv->bind_pipeline({ 7, 1 });
	expect_only(state(), vl::rule::invalid_pipeline, 2);

	pipeline_desc desc = pipeline_for_pass();
	const pipeline_handle real = drv->create_pipeline(desc);
	drv->bind_pipeline({ real.index, real.gen + 1 });
	expect_only(state(), vl::rule::invalid_pipeline, 3);
	BOOST_TEST(inner().calls().pipeline_binds == 0u);
}

BOOST_AUTO_TEST_CASE(APipelineWithoutAShaderIsReportedAndNotCreated)
{
	const pipeline_handle handle = drv->create_pipeline(pipeline_desc{});

	expect_only(state(), vl::rule::invalid_pipeline);
	BOOST_TEST(!handle);
	BOOST_TEST(inner().pipelines().size() == 0u);
}

BOOST_AUTO_TEST_CASE(APipelineOfADeadShaderIsReportedAsADeadObject)
{
	program_object = drv->create_shader(program());
	pipeline_desc desc;
	desc.shader = program_object.get();
	program_object.reset();
	const pipeline_handle handle = drv->create_pipeline(desc);

	expect_only(state(), vl::rule::dead_object);
	BOOST_TEST(!handle);
	BOOST_TEST(inner().pipelines().size() == 0u);
}

BOOST_AUTO_TEST_CASE(BindingAPipelineWhoseShaderWasDeletedIsReportedAndNotForwarded)
{
	open_pass();
	const pipeline_handle handle = drv->create_pipeline(pipeline_for_pass());
	BOOST_TEST(state().total() == 0u);
	program_object.reset();
	drv->bind_pipeline(handle);

	expect_only(state(), vl::rule::dead_object);
	BOOST_TEST(inner().calls().pipeline_binds == 0u);
}

BOOST_AUTO_TEST_CASE(BlendingMoreTargetsThanTheFormatsDeclareIsReported)
{
	open_pass();
	pipeline_desc desc = pipeline_for_pass();
	desc.blend = { blend_state{}, blend_state{} };
	drv->create_pipeline(desc);

	expect_only(state(), vl::rule::invalid_pipeline);
}

BOOST_AUTO_TEST_CASE(TheWrappedDriverGetsItsOwnShaderInThePipeline)
{
	open_pass();
	const pipeline_handle handle = drv->create_pipeline(pipeline_for_pass());

	const auto* wrapper = dynamic_cast<const vl::shader*>(drv->pipeline_info(handle).shader.object);
	BOOST_REQUIRE(wrapper);
	BOOST_TEST(inner().pipeline_info(handle).shader.object == &wrapper->inner());
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(OneDescriptionGivesOneHandleThroughTheDecorator)
{
	open_pass();
	const pipeline_desc desc = pipeline_for_pass();
	const pipeline_handle first = drv->create_pipeline(desc);
	const pipeline_handle again = drv->create_pipeline(desc);
	pipeline_desc other = desc;
	other.cull.enabled = true;
	const pipeline_handle different = drv->create_pipeline(other);

	BOOST_CHECK(first == again);
	BOOST_CHECK(first != different);
	BOOST_TEST(inner().pipelines().size() == 2u);
	BOOST_TEST(state().current_frame().pipelines == 2u);
	BOOST_TEST(state().total() == 0u);
}

BOOST_AUTO_TEST_CASE(PipelineBudgetCountsPipelinesCreatedInAFrame)
{
	vl::swapchain limited(std::make_unique<null::swapchain>(), vl::limits{ .max_pipelines = 2 });
	auto limited_drv = limited.create_driver();
	auto shader = limited_drv->create_shader(program());

	pipeline_desc desc;
	desc.shader = shader.get();
	for (int i = 0; i < 4; ++i) {
		desc.topology = static_cast<RENDER_MODE>(i);
		limited_drv->create_pipeline(desc);
	}
	expect_only(limited.state(), vl::rule::budget_pipelines);
	BOOST_TEST(limited.state().current_frame().pipelines == 4u);

	limited.present();
	for (int i = 0; i < 4; ++i) {
		desc.topology = static_cast<RENDER_MODE>(i);
		limited_drv->create_pipeline(desc);
	}
	BOOST_TEST(limited.state().current_frame().pipelines == 0u);
	BOOST_TEST(limited.state().total() == 1u);
}

BOOST_AUTO_TEST_SUITE_END()
