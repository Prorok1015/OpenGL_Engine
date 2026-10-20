#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include "rnd_null_swapchain.h"
#include <snk/driver/rnd_texture_owner.h>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

// Contract of the null backend: what the engine may rely on from any driver_interface implementation
// that is not a GPU (objects keep their state, lifetimes are counted, bad shaders are rejected).

namespace {

using namespace rnd::driver;

shader_header stage(const char* title, std::string body, shader_header::shader_type type)
{
	shader_header header;
	header.title = title;
	header.body = std::move(body);
	header.type = type;
	return header;
}

std::vector<shader_header> valid_program()
{
	return {
		stage("a.vert", "#version 450 core\nvoid main() {}\n", shader_header::shader_type::VERTEX),
		stage("a.frag", "\n  #version 450 core\nvoid main() {}\n", shader_header::shader_type::FRAGMENT),
	};
}

texture_header target_of(uint32_t size, texture_usage usage, uint32_t mips = 1)
{
	texture_header header;
	header.data.extent.width = size;
	header.data.extent.height = size;
	header.data.mip_levels = mips;
	header.usage = usage;
	return header;
}

render_pass_desc pass_of(const texture_interface* color, const texture_interface* depth = nullptr)
{
	render_pass_desc pass;
	if (color) {
		pass.colors.push_back({ .texture = handle_of(color) });
	}
	if (depth) {
		pass.depth_stencil = attachment_desc{ .texture = handle_of(depth) };
	}
	return pass;
}

void run_pass(null::driver& driver, const render_pass_desc& pass)
{
	driver.begin_render_pass(pass);
	driver.end_render_pass();
}

}

BOOST_AUTO_TEST_SUITE(RndNullDriverTests)

BOOST_AUTO_TEST_CASE(SwapchainPresentCountsFrames)
{
	null::swapchain swapchain;
	BOOST_TEST(swapchain.frames_presented() == 0u);
	swapchain.present();
	swapchain.present();
	swapchain.present();
	BOOST_TEST(swapchain.frames_presented() == 3u);

	swapchain.set_vsync(true);
	BOOST_TEST(swapchain.vsync());
}

BOOST_AUTO_TEST_CASE(SwapchainCreatesANullDriver)
{
	null::swapchain swapchain;
	auto driver = swapchain.create_driver();
	BOOST_REQUIRE(driver);
	BOOST_TEST(static_cast<bool>(dynamic_cast<null::driver*>(driver.get())));
}

BOOST_AUTO_TEST_CASE(ObjectsAreCountedFromCreationToDestruction)
{
	null::driver driver;
	{
		auto shader = driver.create_shader(valid_program());
		auto texture = make_texture(driver, texture_header{});
		auto va = driver.create_vertex_array();
		auto vbo = driver.create_buffer();

		const auto& stats = driver.stats();
		BOOST_TEST(stats.shaders.created == 1u);
		BOOST_TEST(stats.textures.created == 1u);
		BOOST_TEST(stats.vertex_arrays.created == 1u);
		BOOST_TEST(stats.buffers.created == 1u);
		BOOST_TEST(stats.live_total() == 4u);

		texture.reset();
		BOOST_TEST(stats.textures.destroyed == 1u);
		BOOST_TEST(stats.live_total() == 3u);
	}
	BOOST_TEST(driver.stats().live_total() == 0u);
	BOOST_TEST(driver.stats().textures.created == 1u);
}

BOOST_AUTO_TEST_CASE(ObjectsMayOutliveTheDriver)
{
	std::unique_ptr<buffer_interface> buffer;
	{
		null::driver driver;
		buffer = driver.create_buffer();
	}
	BOOST_REQUIRE(buffer);
	buffer.reset();
}

BOOST_AUTO_TEST_CASE(TextureKeepsItsHeader)
{
	null::driver driver;
	texture_header header;
	header.name = "albedo";
	header.data.extent.width = 64;
	header.data.extent.height = 32;
	header.data.format = texture_header::TYPE::RGBA16F;
	header.usage = texture_usage::color_target;

	auto texture = make_texture(driver, header);
	BOOST_REQUIRE(texture);
	BOOST_TEST(texture->width() == 64u);
	BOOST_TEST(texture->height() == 32u);
	BOOST_TEST((texture->get_usage() == texture_usage::color_target));

	const auto* null_texture = dynamic_cast<null::texture*>(texture.get());
	BOOST_REQUIRE(null_texture);
	BOOST_TEST(null_texture->get_header().name == "albedo");
	BOOST_TEST((null_texture->get_header().data.format == texture_header::TYPE::RGBA16F));
}

BOOST_AUTO_TEST_CASE(TexturesAreNamedByHandlesThatGoStaleWithTheTexture)
{
	null::driver driver;
	texture_header header;
	header.name = "target";
	header.data.extent.width = 8;
	header.data.extent.height = 4;
	header.usage = texture_usage::color_target;

	auto first = make_texture(driver, header);
	BOOST_REQUIRE(first);
	const texture_handle first_handle = first->handle();
	BOOST_TEST(static_cast<bool>(first_handle));
	BOOST_TEST(driver.is_alive(first_handle));
	BOOST_TEST(driver.live_textures() == 1u);
	BOOST_TEST(driver.info(first_handle).name == "target");
	BOOST_TEST(driver.info(first_handle).data.extent.width == 8u);
	BOOST_TEST((driver.texture_of(first_handle) == first.get()));

	// A second texture gets another slot.
	auto second = make_texture(driver, header);
	BOOST_TEST((second->handle() != first_handle));
	BOOST_TEST(driver.live_textures() == 2u);

	first.reset();
	BOOST_TEST(!driver.is_alive(first_handle));
	BOOST_TEST(driver.live_textures() == 1u);
	BOOST_TEST((driver.texture_of(first_handle) == nullptr));
	BOOST_TEST(driver.info(first_handle).name == "unknown");

	// The freed slot comes back with the next generation: the old handle still names nothing.
	auto third = make_texture(driver, header);
	BOOST_TEST(third->handle().index == first_handle.index);
	BOOST_TEST(third->handle().gen == first_handle.gen + 1);
	BOOST_TEST(!driver.is_alive(first_handle));
	BOOST_TEST(driver.is_alive(third->handle()));
}

BOOST_AUTO_TEST_CASE(TexturesStillAliveAreFreedWithTheDriver)
{
	// The driver owns its textures: nothing outlives it, and its handles name nothing in another driver.
	texture_handle handle;
	std::shared_ptr<const null::object_stats> stats;
	{
		null::driver driver;
		stats = driver.shared_stats();
		handle = driver.create_texture(texture_header{});
		BOOST_TEST(static_cast<bool>(handle));
		BOOST_TEST(driver.is_alive(handle));
		BOOST_TEST(driver.live_textures() == 1u);
		BOOST_TEST(stats->textures.destroyed == 0u);
	}
	BOOST_TEST(stats->textures.created == 1u);
	BOOST_TEST(stats->textures.destroyed == 1u);
	BOOST_TEST(stats->textures.live() == 0u);
	null::driver other;
	BOOST_TEST(!other.is_alive(handle));
}

BOOST_AUTO_TEST_CASE(DestroyFreesTheTextureAndTheHandleGoesStale)
{
	null::driver driver;
	const texture_handle handle = driver.create_texture(texture_header{});
	BOOST_REQUIRE(driver.is_alive(handle));
	BOOST_TEST((driver.resolve(handle) != nullptr));
	BOOST_TEST(driver.stats().textures.live() == 1u);

	driver.destroy(handle);
	BOOST_TEST(!driver.is_alive(handle));
	BOOST_TEST((driver.resolve(handle) == nullptr));
	BOOST_TEST(driver.live_textures() == 0u);
	BOOST_TEST(driver.stats().textures.destroyed == 1u);

	// A second destroy, or one of the null handle, changes nothing.
	driver.destroy(handle);
	driver.destroy(texture_handle{});
	BOOST_TEST(driver.stats().textures.destroyed == 1u);
}

BOOST_AUTO_TEST_CASE(AHandleOfAnotherDriverNamesNoTexture)
{
	null::driver first;
	null::driver second;
	const texture_handle mine = first.create_texture(texture_header{});
	const texture_handle theirs = second.create_texture(texture_header{});

	// Same slot, different driver id in the generation.
	BOOST_TEST(mine.index == theirs.index);
	BOOST_TEST((mine.gen != theirs.gen));
	BOOST_TEST(first.is_alive(mine));
	BOOST_TEST(!first.is_alive(theirs));
	BOOST_TEST((first.resolve(theirs) == nullptr));

	// Destroying a foreign handle frees nothing.
	first.destroy(theirs);
	BOOST_TEST(second.is_alive(theirs));
	BOOST_TEST(first.is_alive(mine));
}

BOOST_AUTO_TEST_CASE(BufferKeepsSizeAndData)
{
	null::driver driver;
	auto buffer = driver.create_buffer();
	const std::vector<float> data{ 1.f, 2.f, 3.f, 4.f };
	buffer->set_data(data, BUFFER_BINDING::STATIC);

	const auto* null_buffer = dynamic_cast<null::buffer*>(buffer.get());
	BOOST_REQUIRE(null_buffer);
	BOOST_TEST(null_buffer->size() == sizeof(float) * 4);
	BOOST_TEST((null_buffer->binding() == BUFFER_BINDING::STATIC));
	float second = 0.f;
	std::memcpy(&second, null_buffer->bytes().data() + sizeof(float), sizeof(float));
	BOOST_TEST(second == 2.f);

	buffer->set_layout(BufferLayout{ BufferElement{ SHADER_DATA_TYPE::VEC4_F, "value" } });
	BOOST_TEST(buffer->get_layout().get_stride() == sizeof(float) * 4);
}

BOOST_AUTO_TEST_CASE(ReservedBufferIsAllocatedAndZeroed)
{
	null::driver driver;
	auto buffer = driver.create_buffer();
	buffer->reserve(32);

	const auto* null_buffer = dynamic_cast<null::buffer*>(buffer.get());
	BOOST_REQUIRE(null_buffer);
	BOOST_TEST(null_buffer->size() == 32u);
	BOOST_TEST(std::to_integer<int>(null_buffer->bytes().front()) == 0);
}

BOOST_AUTO_TEST_CASE(GpuBuffersKeepSizeAndWrittenData)
{
	null::driver driver;
	const buffer_handle ubo = driver.create_buffer({ .size = 16, .usage = buffer_usage::uniform });
	const int value = 42;
	driver.update_buffer(ubo, 8, std::as_bytes(std::span{ &value, 1 }));

	const buffer_desc* info = driver.buffer_info(ubo);
	BOOST_REQUIRE(info);
	BOOST_TEST(info->size == 16u);
	const auto* bytes = driver.buffer_bytes(ubo);
	BOOST_REQUIRE(bytes);
	BOOST_REQUIRE(bytes->size() == 16u);
	int stored = 0;
	std::memcpy(&stored, bytes->data() + 8, sizeof(stored));
	BOOST_TEST(stored == 42);

	const buffer_handle ssbo = driver.create_buffer({ .size = 64, .usage = buffer_usage::storage });
	BOOST_TEST(driver.buffer_info(ssbo)->size == 64u);
	BOOST_TEST(driver.live_buffers() == 2u);
	driver.destroy_buffer(ssbo);
	BOOST_TEST(driver.live_buffers() == 1u);
	BOOST_TEST(driver.buffer_info(ssbo) == nullptr);
}

BOOST_AUTO_TEST_CASE(VertexArrayKeepsItsBuffers)
{
	null::driver driver;
	auto va = driver.create_vertex_array();
	std::shared_ptr<buffer_interface> vbo = driver.create_buffer();
	std::shared_ptr<buffer_interface> ibo = driver.create_buffer();
	va->add_vertex_buffer(vbo);
	va->set_index_buffer(ibo);

	const auto* null_va = dynamic_cast<null::vertex_array*>(va.get());
	BOOST_REQUIRE(null_va);
	BOOST_REQUIRE(null_va->vertex_buffers().size() == 1u);
	BOOST_TEST(null_va->vertex_buffers().front() == vbo);
	BOOST_TEST(null_va->index_buffer() == ibo);
}

BOOST_AUTO_TEST_CASE(ShaderKeepsItsHeaders)
{
	null::driver driver;
	auto shader = driver.create_shader(valid_program());
	BOOST_REQUIRE(shader);

	const auto* null_shader = dynamic_cast<null::shader*>(shader.get());
	BOOST_REQUIRE(null_shader);
	BOOST_REQUIRE(null_shader->headers().size() == 2u);
	BOOST_TEST(null_shader->headers().front().title == "a.vert");
}

BOOST_AUTO_TEST_CASE(BindingAPipelineMakesItsShaderTheOneDrawsUse)
{
	null::driver driver;
	auto shader = driver.create_shader(valid_program());
	pipeline_desc desc;
	desc.shader = shader.get();
	const pipeline_handle pipeline = driver.create_pipeline(desc);

	BOOST_TEST(driver.calls().pipeline_binds == 0u);
	driver.bind_pipeline(pipeline);

	BOOST_TEST(driver.calls().pipeline_binds == 1u);
	BOOST_CHECK(driver.bound_pipeline() == pipeline);
	BOOST_TEST(driver.pipeline_info(driver.bound_pipeline()).shader.object == shader.get());
}

BOOST_AUTO_TEST_CASE(ShaderWithoutStagesIsRejected)
{
	null::driver driver;
	BOOST_TEST(!driver.create_shader({}));
	BOOST_TEST(driver.stats().shaders.created == 0u);
}

BOOST_AUTO_TEST_CASE(EmptyShaderStageIsRejected)
{
	null::driver driver;
	auto headers = valid_program();
	headers.back().body = " \n\t\r\n";
	BOOST_TEST(!driver.create_shader(headers));
	BOOST_TEST(driver.stats().shaders.created == 0u);
}

BOOST_AUTO_TEST_CASE(ShaderStageWithoutVersionFirstIsRejected)
{
	null::driver driver;

	auto no_version = valid_program();
	no_version.front().body = "void main() {}\n";
	BOOST_TEST(!driver.create_shader(no_version));

	auto version_late = valid_program();
	version_late.back().body = "#define X\n#version 450 core\nvoid main() {}\n";
	BOOST_TEST(!driver.create_shader(version_late));
	BOOST_TEST(driver.stats().shaders.created == 0u);
}

BOOST_AUTO_TEST_CASE(ValidateShaderHeadersNamesTheReason)
{
	BOOST_TEST(null::validate_shader_headers(valid_program()).empty());
	BOOST_TEST(!null::validate_shader_headers({}).empty());

	auto headers = valid_program();
	headers.front().body = "void main() {}";
	BOOST_TEST(null::validate_shader_headers(headers).find("a.vert") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(RenderPassesAreCountedAndTheLastOneIsKept)
{
	null::driver driver;
	auto color = make_texture(driver, target_of(32, texture_usage::color_target));
	BOOST_TEST(!driver.in_render_pass());

	render_pass_desc pass = pass_of(color.get());
	pass.colors[0].load = load_op::clear;
	pass.render_area = { 0, 0, 16, 16 };
	pass.debug_name = "probe";
	driver.begin_render_pass(pass);
	BOOST_TEST(driver.in_render_pass());
	driver.end_render_pass();

	BOOST_TEST(!driver.in_render_pass());
	BOOST_TEST(driver.calls().render_pass_begins == 1u);
	BOOST_TEST(driver.calls().render_pass_ends == 1u);
	BOOST_TEST(driver.last_render_pass().colors.size() == 1u);
	BOOST_TEST((driver.last_render_pass().colors[0].load == load_op::clear));
	BOOST_TEST((driver.last_render_pass().render_area == glm::ivec4{ 0, 0, 16, 16 }));
	BOOST_TEST(std::string(driver.last_render_pass().debug_name) == "probe");
}

BOOST_AUTO_TEST_CASE(PassesWithTheSameAttachmentsShareOneFramebuffer)
{
	null::driver driver;
	auto color = make_texture(driver, target_of(32, texture_usage::color_target));
	auto depth = make_texture(driver, target_of(32, texture_usage::depth_target));
	const auto& fbos = driver.stats().framebuffers;

	for (int i = 0; i < 3; ++i) {
		run_pass(driver, pass_of(color.get(), depth.get()));
	}
	BOOST_TEST(fbos.size() == 1u);
	BOOST_TEST(fbos.misses() == 1u);
	BOOST_TEST(fbos.hits() == 2u);

	// Load and store ops, the render area and the label are not part of the identity.
	render_pass_desc other_ops = pass_of(color.get(), depth.get());
	other_ops.colors[0].load = load_op::clear;
	other_ops.depth_stencil->store = store_op::dont_care;
	other_ops.render_area = { 0, 0, 8, 8 };
	other_ops.debug_name = "other";
	run_pass(driver, other_ops);
	BOOST_TEST(fbos.size() == 1u);
	BOOST_TEST(fbos.hits() == 3u);
}

BOOST_AUTO_TEST_CASE(AnotherAttachmentMipOrLayerOrSetBuildsAnotherFramebuffer)
{
	null::driver driver;
	auto color = make_texture(driver, target_of(32, texture_usage::color_target, 3));
	auto second = make_texture(driver, target_of(32, texture_usage::color_target));
	auto depth = make_texture(driver, target_of(32, texture_usage::depth_target));
	const auto& fbos = driver.stats().framebuffers;

	run_pass(driver, pass_of(color.get()));
	render_pass_desc mip1 = pass_of(color.get());
	mip1.colors[0].mip = 1;
	run_pass(driver, mip1);
	render_pass_desc layer1 = pass_of(color.get());
	layer1.colors[0].layer = 1;
	run_pass(driver, layer1);
	run_pass(driver, pass_of(color.get(), depth.get()));
	run_pass(driver, pass_of(second.get()));
	BOOST_TEST(fbos.misses() == 5u);
	BOOST_TEST(fbos.hits() == 0u);

	// A depth-only pass and one with an unused color slot are different framebuffers.
	run_pass(driver, pass_of(nullptr, depth.get()));
	render_pass_desc hole = pass_of(nullptr, depth.get());
	hole.colors.push_back({});
	run_pass(driver, hole);
	BOOST_TEST(fbos.misses() == 7u);
}

BOOST_AUTO_TEST_CASE(LayeredAndSingleLayerAttachmentsOfOneTextureAreDifferentFramebuffers)
{
	null::driver driver;
	texture_header header = target_of(32, texture_usage::color_target);
	header.type = TEXTURE_TYPE::TEXTURE_2D_ARRAY;
	header.data.extent.layers = 4;
	auto array = make_texture(driver, header);
	const auto& fbos = driver.stats().framebuffers;

	render_pass_desc layered = pass_of(array.get());
	BOOST_TEST(layered.colors[0].layer == attachment_desc::all_layers);
	render_pass_desc layer0 = pass_of(array.get());
	layer0.colors[0].layer = 0;
	render_pass_desc layer2 = pass_of(array.get());
	layer2.colors[0].layer = 2;

	run_pass(driver, layered);
	run_pass(driver, layer0);
	run_pass(driver, layer2);
	BOOST_TEST(fbos.misses() == 3u);
	run_pass(driver, layered);
	run_pass(driver, layer0);
	BOOST_TEST(fbos.hits() == 2u);
	BOOST_TEST(fbos.size() == 3u);
}

BOOST_AUTO_TEST_CASE(BackbufferPassesNeedNoFramebuffer)
{
	null::driver driver;
	render_pass_desc window;
	window.backbuffer = true;
	window.colors.push_back({ .load = load_op::clear });
	run_pass(driver, window);
	run_pass(driver, window);

	BOOST_TEST(driver.calls().render_pass_begins == 2u);
	BOOST_TEST(driver.stats().framebuffers.size() == 0u);
	BOOST_TEST(driver.stats().framebuffers.misses() == 0u);
}

BOOST_AUTO_TEST_CASE(DestroyingATextureEvictsTheFramebuffersBuiltFromIt)
{
	null::driver driver;
	auto first = make_texture(driver, target_of(32, texture_usage::color_target));
	auto second = make_texture(driver, target_of(32, texture_usage::color_target));
	auto depth = make_texture(driver, target_of(32, texture_usage::depth_target));
	const auto& fbos = driver.stats().framebuffers;

	run_pass(driver, pass_of(first.get(), depth.get()));
	run_pass(driver, pass_of(second.get(), depth.get()));
	run_pass(driver, pass_of(second.get()));
	BOOST_TEST(fbos.size() == 3u);

	first.reset();
	BOOST_TEST(fbos.size() == 2u);

	depth.reset();
	BOOST_TEST(fbos.size() == 1u);

	second.reset();
	BOOST_TEST(fbos.size() == 0u);
}

BOOST_AUTO_TEST_CASE(ARecreatedTextureNeverMatchesAnEvictedFramebuffer)
{
	null::driver driver;
	const auto& fbos = driver.stats().framebuffers;

	for (int generation = 0; generation < 4; ++generation) {
		// The allocator is free to hand the same address out again.
		auto color = make_texture(driver, target_of(32, texture_usage::color_target));
		run_pass(driver, pass_of(color.get()));
		run_pass(driver, pass_of(color.get()));
	}
	BOOST_TEST(fbos.misses() == 4u);
	BOOST_TEST(fbos.hits() == 4u);
	BOOST_TEST(fbos.size() == 0u);
}

BOOST_AUTO_TEST_CASE(StateAndCallsAreRecorded)
{
	null::driver driver;
	driver.set_viewport({ 0, 0, 640, 480 });
	driver.set_line_size(2.f);
	auto shader = driver.create_shader(valid_program());
	pipeline_desc desc;
	desc.shader = shader.get();
	desc.blend = { blend_state{ .enabled = true } };
	driver.bind_pipeline(driver.create_pipeline(desc));

	auto va = driver.create_vertex_array();
	driver.draw_indices(va, 36);
	desc.topology = RENDER_MODE::LINE;
	driver.bind_pipeline(driver.create_pipeline(desc));
	driver.draw_indices(va.get(), 2, 0, 0);
	driver.invalidate_state_cache();

	BOOST_TEST((driver.viewport() == glm::ivec4{ 0, 0, 640, 480 }));
	BOOST_TEST(driver.line_size() == 2.f);
	BOOST_TEST(driver.pipeline_info(driver.bound_pipeline()).blend.size() == 1u);
	BOOST_TEST(driver.calls().pipeline_binds == 2u);
	BOOST_TEST(driver.calls().draws == 2u);
	BOOST_TEST(driver.draws_with(RENDER_MODE::TRIANGLE) == 1u);
	BOOST_TEST(driver.draws_with(RENDER_MODE::LINE) == 1u);
	BOOST_TEST(driver.calls().state_cache_invalidations == 1u);
}

struct pipeline_fixture
{
	pipeline_fixture()
		: shader(driver.create_shader(valid_program()))
		, other_shader(driver.create_shader(valid_program()))
	{
		base.shader = shader.get();
		base.depth = { .enabled = true, .write_mask = true, .test_func = depth_state::func::LEQUAL };
		base.color_formats = { texture_format::RGBA8 };
		base.depth_format = texture_format::D32;
	}

	null::driver driver;
	std::unique_ptr<shader_interface> shader;
	std::unique_ptr<shader_interface> other_shader;
	pipeline_desc base;
};

BOOST_FIXTURE_TEST_SUITE(NullDriverPipelines, pipeline_fixture)

BOOST_AUTO_TEST_CASE(OneDescriptionGivesOneHandle)
{
	const pipeline_handle first = driver.create_pipeline(base);
	const pipeline_handle again = driver.create_pipeline(pipeline_desc{ base });
	pipeline_desc rebuilt;
	rebuilt.shader = shader.get();
	rebuilt.depth = { .enabled = true, .write_mask = true, .test_func = depth_state::func::LEQUAL };
	rebuilt.color_formats = { texture_format::RGBA8 };
	rebuilt.depth_format = texture_format::D32;
	const pipeline_handle equal = driver.create_pipeline(rebuilt);

	BOOST_TEST(static_cast<bool>(first));
	BOOST_CHECK(first == again);
	BOOST_CHECK(first == equal);
	BOOST_TEST(driver.pipelines().size() == 1u);
	BOOST_TEST(driver.pipelines().misses() == 1u);
	BOOST_TEST(driver.pipelines().hits() == 2u);
}

BOOST_AUTO_TEST_CASE(EveryFieldOfTheDescriptionTellsPipelinesApart)
{
	std::vector<pipeline_desc> variants;
	const auto vary = [&](auto&& change) {
		pipeline_desc desc = base;
		change(desc);
		variants.push_back(desc);
	};
	vary([&](pipeline_desc& d) { d.shader = other_shader.get(); });
	vary([](pipeline_desc& d) { d.topology = RENDER_MODE::LINE; });
	vary([](pipeline_desc& d) { d.topology = RENDER_MODE::TRIANGLE_STRIP; });
	vary([](pipeline_desc& d) { d.topology = RENDER_MODE::LINE_STRIP; });
	vary([](pipeline_desc& d) { d.topology = RENDER_MODE::POINT; });
	vary([](pipeline_desc& d) { d.depth.enabled = false; });
	vary([](pipeline_desc& d) { d.depth.write_mask = false; });
	vary([](pipeline_desc& d) { d.depth.test_func = depth_state::func::ALWAYS; });
	vary([](pipeline_desc& d) { d.cull.enabled = true; });
	vary([](pipeline_desc& d) { d.cull = { .enabled = true, .cull_mode = face_culling_state::mode::FRONT }; });
	vary([](pipeline_desc& d) { d.blend = { blend_state{} }; });
	vary([](pipeline_desc& d) { d.blend = { blend_state{ .enabled = true } }; });
	vary([](pipeline_desc& d) { d.blend = { blend_state{ .enabled = true, .src_factor = blend_state::factor::SRC_ALPHA } }; });
	vary([](pipeline_desc& d) { d.blend = { blend_state{ .enabled = true, .dst_factor = blend_state::factor::ONE } }; });
	vary([](pipeline_desc& d) { d.blend = { blend_state{ .enabled = true, .blend_eq = blend_state::equation::MAX } }; });
	vary([](pipeline_desc& d) { d.blend = { blend_state{}, blend_state{} }; });
	vary([](pipeline_desc& d) { d.color_formats = { texture_format::RGBA16F }; });
	vary([](pipeline_desc& d) { d.color_formats = { texture_format::RGBA8, texture_format::R8 }; });
	vary([](pipeline_desc& d) { d.color_formats = { std::nullopt }; });
	vary([](pipeline_desc& d) { d.color_formats.clear(); });
	vary([](pipeline_desc& d) { d.depth_format = texture_format::D24_S8; });
	vary([](pipeline_desc& d) { d.depth_format.reset(); });

	std::vector<pipeline_handle> handles{ driver.create_pipeline(base) };
	for (const pipeline_desc& desc : variants) {
		const pipeline_handle handle = driver.create_pipeline(desc);
		for (const pipeline_handle& seen : handles) {
			BOOST_CHECK(handle != seen);
		}
		handles.push_back(handle);
	}
	BOOST_TEST(driver.pipelines().size() == variants.size() + 1);
	BOOST_TEST(driver.pipelines().hits() == 0u);
}

BOOST_AUTO_TEST_CASE(APipelineCannotChangeAfterItIsCreated)
{
	pipeline_desc written = base;
	const pipeline_handle pipeline = driver.create_pipeline(written);

	// The description was copied: the caller's copy is free to change without touching the pipeline.
	written.depth.enabled = false;
	written.topology = RENDER_MODE::POINT;
	written.blend = { blend_state{ .enabled = true } };

	BOOST_CHECK(driver.pipeline_info(pipeline) == base);
	BOOST_CHECK(driver.create_pipeline(base) == pipeline);
	BOOST_CHECK(driver.create_pipeline(written) != pipeline);
	BOOST_CHECK(driver.pipeline_info(pipeline) == base);

	// The interface offers a read-only view and a handle that carries no state to edit.
	static_assert(std::is_same_v<decltype(std::declval<const driver_interface&>().pipeline_info(pipeline_handle{})), const pipeline_desc&>);
	static_assert(sizeof(pipeline_handle) == 8);
	static_assert(std::is_trivially_copyable_v<pipeline_handle>);
	static_assert(!std::is_convertible_v<pipeline_handle, bool>);
	static_assert(!std::is_convertible_v<pipeline_handle, const pipeline_desc&>);
}

BOOST_AUTO_TEST_CASE(AHandleOfNoPipelineDescribesAnEmptyOne)
{
	BOOST_CHECK(driver.pipeline_info({}) == pipeline_desc{});
	BOOST_CHECK(driver.pipeline_info({ 5, 1 }) == pipeline_desc{});
	const pipeline_handle pipeline = driver.create_pipeline(base);
	BOOST_CHECK(driver.pipeline_info({ pipeline.index, pipeline.gen + 1 }) == pipeline_desc{});
	BOOST_CHECK(!driver.bound_pipeline());
}

BOOST_AUTO_TEST_CASE(TheDescriptionStaysReadableWhileTheCacheGrows)
{
	const pipeline_handle first = driver.create_pipeline(base);
	const pipeline_desc& view = driver.pipeline_info(first);
	for (int i = 0; i < 64; ++i) {
		pipeline_desc other = base;
		other.blend = { blend_state{ .enabled = true, .src_factor = blend_state::factor::SRC_ALPHA } };
		other.color_formats.clear();
		for (int c = 0; c < i % 8; ++c) {
			other.color_formats.push_back(texture_format::R8);
		}
		other.depth.write_mask = (i % 2) == 0;
		other.cull.enabled = (i % 3) == 0;
		other.topology = static_cast<RENDER_MODE>(i % 5);
		driver.create_pipeline(other);
	}
	BOOST_CHECK(view == base);
}

BOOST_AUTO_TEST_CASE(PipelineFormatsFollowThePassTargets)
{
	auto color = make_texture(driver, target_of(16, texture_usage::color_target));
	texture_header depth_header = target_of(16, texture_usage::depth_target);
	depth_header.data.format = texture_header::TYPE::D32F;
	auto depth = make_texture(driver, depth_header);

	pipeline_desc desc = base;
	set_target_formats(driver, desc, pass_of(color.get(), depth.get()));
	BOOST_REQUIRE(desc.color_formats.size() == 1u);
	BOOST_TEST((*desc.color_formats[0] == texture_format::RGBA8));
	BOOST_TEST((desc.depth_format == texture_format::D32F));

	set_target_formats(driver, desc, pass_of(color.get()));
	BOOST_TEST(!desc.depth_format.has_value());

	render_pass_desc holes = pass_of(nullptr);
	holes.colors.push_back({});
	set_target_formats(driver, desc, holes);
	BOOST_REQUIRE(desc.color_formats.size() == 1u);
	BOOST_TEST(!desc.color_formats[0].has_value());

	render_pass_desc window;
	window.backbuffer = true;
	set_target_formats(driver, desc, window);
	BOOST_REQUIRE(desc.color_formats.size() == 1u);
	BOOST_TEST((*desc.color_formats[0] == backbuffer_color_format));
	BOOST_TEST((desc.depth_format == backbuffer_depth_format));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE_END()
