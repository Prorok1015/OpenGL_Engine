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
	header.usage = TEXTURE_USAGE::COLOR_TARGET;

	auto texture = driver.create_texture(header);
	BOOST_REQUIRE(texture);
	BOOST_TEST(texture->width() == 64u);
	BOOST_TEST(texture->height() == 32u);
	BOOST_TEST((texture->get_usage() == TEXTURE_USAGE::COLOR_TARGET));

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

BOOST_AUTO_TEST_CASE(FrameBufferStackStartsAtTheBackbuffer)
{
	null::driver driver;
	BOOST_TEST(driver.frame_buffer_depth() == 1u);
	driver.push_frame_buffer();
	driver.push_frame_buffer();
	BOOST_TEST(driver.frame_buffer_depth() == 3u);
	driver.pop_frame_buffer();
	driver.pop_frame_buffer();
	BOOST_TEST(driver.frame_buffer_depth() == 1u);
	BOOST_TEST(driver.calls().push_frame_buffers == 2u);
	BOOST_TEST(driver.calls().pop_frame_buffers == 2u);
}

BOOST_AUTO_TEST_CASE(StateAndCallsAreRecorded)
{
	null::driver driver;
	driver.set_viewport({ 0, 0, 640, 480 });
	driver.set_scissor({ 1, 2, 3, 4 });
	driver.set_line_size(2.f);
	driver.set_render_state(render_state::transparent());
	driver.clear(CLEAR_FLAGS::COLOR_BUFFER);

	auto va = driver.create_vertex_array();
	driver.draw_indices(va, RENDER_MODE::TRIANGLE, 36);
	driver.draw_indices(va.get(), RENDER_MODE::LINE, 2, 0, 0);

	BOOST_TEST((driver.viewport() == glm::ivec4{ 0, 0, 640, 480 }));
	BOOST_TEST((driver.scissor() == glm::ivec4{ 1, 2, 3, 4 }));
	BOOST_TEST(driver.line_size() == 2.f);
	BOOST_TEST(driver.current_render_state().blend_states.size() == 1u);
	BOOST_TEST(driver.calls().render_state_sets == 1u);
	BOOST_TEST(driver.calls().clears == 1u);
	BOOST_TEST(driver.calls().draws == 2u);
}

BOOST_AUTO_TEST_SUITE_END()
