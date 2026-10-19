#include <boost/test/unit_test.hpp>
#include "rnd_null_driver.h"
#include "rnd_null_swapchain.h"
#include <cstring>
#include <memory>
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

render_pass_desc pass_of(texture_interface* color, texture_interface* depth = nullptr)
{
	render_pass_desc pass;
	if (color) {
		pass.colors.push_back({ .texture = color });
	}
	if (depth) {
		pass.depth_stencil = attachment_desc{ .texture = depth };
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
		auto texture = driver.create_texture(texture_header{});
		auto va = driver.create_vertex_array();
		auto vbo = driver.create_buffer();
		auto ubo = driver.create_uniform_buffer(64, 0);
		auto ssbo = driver.create_ssbo_buffer(128, 1);

		const auto& stats = driver.stats();
		BOOST_TEST(stats.shaders.created == 1u);
		BOOST_TEST(stats.textures.created == 1u);
		BOOST_TEST(stats.vertex_arrays.created == 1u);
		BOOST_TEST(stats.buffers.created == 1u);
		BOOST_TEST(stats.uniform_buffers.created == 1u);
		BOOST_TEST(stats.ssbo_buffers.created == 1u);
		BOOST_TEST(stats.live_total() == 6u);

		texture.reset();
		BOOST_TEST(stats.textures.destroyed == 1u);
		BOOST_TEST(stats.live_total() == 5u);
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

	auto texture = driver.create_texture(header);
	BOOST_REQUIRE(texture);
	BOOST_TEST(texture->width() == 64u);
	BOOST_TEST(texture->height() == 32u);
	BOOST_TEST((texture->get_usage() == texture_usage::color_target));

	const auto* null_texture = dynamic_cast<null::texture*>(texture.get());
	BOOST_REQUIRE(null_texture);
	BOOST_TEST(null_texture->get_header().name == "albedo");
	BOOST_TEST((null_texture->get_header().data.format == texture_header::TYPE::RGBA16F));
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

BOOST_AUTO_TEST_CASE(ConstantBuffersKeepSizeAndWrittenData)
{
	null::driver driver;
	auto ubo = driver.create_uniform_buffer(16, 3);
	const int value = 42;
	ubo->set_data(&value, sizeof(value), 8);

	const auto* null_ubo = dynamic_cast<null::uniform_buffer*>(ubo.get());
	BOOST_REQUIRE(null_ubo);
	BOOST_TEST(null_ubo->size() == 16u);
	BOOST_TEST(null_ubo->binding() == 3u);
	int stored = 0;
	std::memcpy(&stored, null_ubo->bytes().data() + 8, sizeof(stored));
	BOOST_TEST(stored == 42);

	auto ssbo = driver.create_ssbo_buffer(64, 2);
	const auto* null_ssbo = dynamic_cast<null::ssbo_buffer*>(ssbo.get());
	BOOST_REQUIRE(null_ssbo);
	BOOST_TEST(null_ssbo->size() == 64u);
	ssbo->bind(5);
	BOOST_TEST(null_ssbo->binding() == 5u);
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

BOOST_AUTO_TEST_CASE(ShaderKeepsItsHeadersAndCountsUse)
{
	null::driver driver;
	auto shader = driver.create_shader(valid_program());
	BOOST_REQUIRE(shader);

	shader->use();
	shader->uniform("model", 1.f);
	shader->uniform("model", glm::mat4{ 1.f });

	const auto* null_shader = dynamic_cast<null::shader*>(shader.get());
	BOOST_REQUIRE(null_shader);
	BOOST_REQUIRE(null_shader->headers().size() == 2u);
	BOOST_TEST(null_shader->headers().front().title == "a.vert");
	BOOST_TEST(null_shader->use_calls() == 1u);
	BOOST_TEST(null_shader->uniform_calls() == 2u);
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
	auto color = driver.create_texture(target_of(32, texture_usage::color_target));
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
	auto color = driver.create_texture(target_of(32, texture_usage::color_target));
	auto depth = driver.create_texture(target_of(32, texture_usage::depth_target));
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
	auto color = driver.create_texture(target_of(32, texture_usage::color_target, 3));
	auto second = driver.create_texture(target_of(32, texture_usage::color_target));
	auto depth = driver.create_texture(target_of(32, texture_usage::depth_target));
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
	auto array = driver.create_texture(header);
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
	auto first = driver.create_texture(target_of(32, texture_usage::color_target));
	auto second = driver.create_texture(target_of(32, texture_usage::color_target));
	auto depth = driver.create_texture(target_of(32, texture_usage::depth_target));
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
		auto color = driver.create_texture(target_of(32, texture_usage::color_target));
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
	driver.set_render_state(render_state::transparent());

	auto va = driver.create_vertex_array();
	driver.draw_indices(va, RENDER_MODE::TRIANGLE, 36);
	driver.draw_indices(va.get(), RENDER_MODE::LINE, 2, 0, 0);

	BOOST_TEST((driver.viewport() == glm::ivec4{ 0, 0, 640, 480 }));
	BOOST_TEST(driver.line_size() == 2.f);
	BOOST_TEST(driver.current_render_state().blend_states.size() == 1u);
	BOOST_TEST(driver.calls().render_state_sets == 1u);
	BOOST_TEST(driver.calls().draws == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
