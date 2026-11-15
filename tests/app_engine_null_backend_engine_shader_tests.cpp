#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include "rnd_debug_pass.h"
#include "rnd_drawing_test_context.hpp"
#include <snk/render/rnd_render_packet.hpp>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory_resource>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The render passes read their shaders from engine:// when it is mounted (#929): a project's res://shaders file at the
// same path is not compiled in their place.

namespace {

namespace fs = std::filesystem;

constexpr std::string_view project_marker = "// project-override";

std::vector<std::byte> project_override_of(std::string_view name)
{
	std::ifstream in(fs::path{ SNK_CORE_RES_DIR } / "shaders" / std::string{ name }, std::ios::binary);
	std::string text{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	text += "\n";
	text += project_marker;
	text += "\n";
	const auto* begin = reinterpret_cast<const std::byte*>(text.data());
	return { begin, begin + text.size() };
}

// The project's res:// holds its own copies of the passes' shaders.
void override_pass_shaders(res::resource_system& rs)
{
	for (const char* name : { "z_prepass.vert", "z_prepass.frag", "composition.vert", "composition.frag", "debug_line.vert", "debug_line.frag" }) {
		rs.store(res::tag{ res::tag::default_protocol(), std::string{ "shaders/" } + name }, project_override_of(name));
	}
}

struct project_overrides_fixture : test_util::null_engine_fixture
{
	explicit project_overrides_fixture(bool mount_engine)
		: null_engine_fixture(true, [mount_engine](res::resource_system& rs) {
			override_pass_shaders(rs);
			if (mount_engine) {
				test_util::mount_dir(rs.registrate_resolver<test_util::ram_resolver>(res::tag::engine), SNK_CORE_RES_DIR, res::tag::engine);
			}
		})
	{
	}

	// The names of the compiled shaders whose source carries the project's marker; `compiled` gets every name.
	std::set<std::string> compiled_with_marker(std::set<std::string>& compiled)
	{
		driver()->record_shader_sources(true);
		auto& levels = storage.require<scn::context_manager>();
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_quads.desc")));
		test_util::pump(tasks);
		BOOST_REQUIRE(run_frames(3) == 3);
		std::set<std::string> marked;
		for (const auto& program : driver()->shader_sources()) {
			for (const auto& header : program) {
				// A variant's title has its defines before the extension: "scene{ A | B }.frag".
				const std::string name = header.title.substr(0, header.title.find_first_of(".{ ")) + header.title.substr(header.title.find_last_of('.'));
				compiled.insert(name);
				if (header.body.find(project_marker) != std::string::npos) {
					marked.insert(name);
				}
			}
		}
		return marked;
	}

	// The same for one frame of a debug pass drawing a line, on a driver of its own.
	std::set<std::string> debug_pass_with_marker(std::set<std::string>& compiled)
	{
		ds::frame_allocator_reset();
		rnd::driver::null::driver drv;
		drv.record_shader_sources(true);
		test_util::drawing_context context(drv);
		std::pmr::vector<rnd::render_packet_t> packets(ds::frame_allocator());
		packets.reserve(1);
		rnd::render_packet_t packet;
		packet.camera.viewport = { 0, 0, 64, 64 };
		packet.camera.color_targets.push_back(res::tag(res::tag::memory, "__engine_shader_debug_rt"));
		packet.debug_draws.lines.push_back(rnd::debug_line_t{ glm::vec3{ 0.0f }, glm::vec3{ 1.0f } });
		packets.push_back(std::move(packet));
		context.use_packets(&packets);

		rnd::debug_pass debug(storage.require<res::resource_system>());
		debug.execute(context, drv);

		std::set<std::string> marked;
		for (const auto& program : drv.shader_sources()) {
			for (const auto& header : program) {
				compiled.insert(header.title);
				if (header.body.find(project_marker) != std::string::npos) {
					marked.insert(header.title);
				}
			}
		}
		return marked;
	}
};

struct with_engine : project_overrides_fixture
{
	with_engine() : project_overrides_fixture(true) {}
};

struct without_engine : project_overrides_fixture
{
	without_engine() : project_overrides_fixture(false) {}
};

}

BOOST_AUTO_TEST_SUITE(AppEngineNullBackendEngineShaders)

BOOST_FIXTURE_TEST_CASE(WithEngineMounted_ThePassesCompileTheEngineShaders, with_engine)
{
	std::set<std::string> compiled;
	const std::set<std::string> marked = compiled_with_marker(compiled);
	BOOST_TEST(compiled.contains("z_prepass.vert"));
	BOOST_TEST(compiled.contains("composition.frag"));
	BOOST_TEST(marked.empty(), "compiled from the project: " << (marked.empty() ? "" : *marked.begin()));
}

// The control: without engine:// the passes read res://, where the project's copies win.
BOOST_FIXTURE_TEST_CASE(WithoutEngine_ThePassesCompileWhatResHolds, without_engine)
{
	std::set<std::string> compiled;
	const std::set<std::string> marked = compiled_with_marker(compiled);
	BOOST_TEST(marked.contains("z_prepass.vert"));
	BOOST_TEST(marked.contains("composition.frag"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(AppEngineNullBackendEngineDebugShaders)

BOOST_FIXTURE_TEST_CASE(WithEngineMounted_TheDebugPassCompilesTheEngineShaders, with_engine)
{
	std::set<std::string> compiled;
	const std::set<std::string> marked = debug_pass_with_marker(compiled);
	BOOST_TEST(compiled.contains("debug_line.vert"));
	BOOST_TEST(compiled.contains("debug_line.frag"));
	BOOST_TEST(marked.empty(), "compiled from the project: " << (marked.empty() ? "" : *marked.begin()));
}

BOOST_FIXTURE_TEST_CASE(WithoutEngine_TheDebugPassCompilesWhatResHolds, without_engine)
{
	std::set<std::string> compiled;
	const std::set<std::string> marked = debug_pass_with_marker(compiled);
	BOOST_TEST(marked.contains("debug_line.vert"));
	BOOST_TEST(marked.contains("debug_line.frag"));
}

BOOST_AUTO_TEST_SUITE_END()
