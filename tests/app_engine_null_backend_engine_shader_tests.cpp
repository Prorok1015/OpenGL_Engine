#include <boost/test/unit_test.hpp>
#include "null_engine_fixture.hpp"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The render passes read their shaders from engine:// when it is mounted (#929): a project's res://shaders file at the
// same path is not compiled in their place. The smoke level has a model, hence the assimp build only.

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
	for (const char* name : { "z_prepass.vert", "z_prepass.frag", "composition.vert", "composition.frag" }) {
		rs.store(res::tag{ res::tag::default_protocol(), std::string{ "shaders/" } + name }, project_override_of(name));
	}
}

struct project_overrides_fixture : test_util::null_engine_fixture
{
	explicit project_overrides_fixture(bool mount_engine)
		: null_engine_fixture(false, [mount_engine](res::resource_system& rs) {
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
		BOOST_REQUIRE(levels.load_level(res::tag::make("levels/smoke_level.desc")));
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

BOOST_AUTO_TEST_SUITE(AppEngineNullBackendEngineShaders, * boost::unit_test::enable_if<(SNK_HAS_ASSIMP != 0)>())

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
