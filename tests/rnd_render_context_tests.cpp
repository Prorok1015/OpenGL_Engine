#include <boost/test/unit_test.hpp>
// Only the public render headers a game includes, nothing that names a manager: the checks below are about what
// these headers make visible.
#include <snk/render/rnd_frame_context.h>
#include <snk/render/rnd_render_context.h>
#include <snk/render/rnd_render_pass_interface.h>
#include <snk/render/rnd_render_system.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#ifndef SNK_RENDER_SRC_DIR
#error "SNK_RENDER_SRC_DIR must point at core/engine/render/render_system"
#endif

// The passes get a render_context, which has none of the managers (#212). Two kinds of check: the compiler's, on
// what the public headers declare, and one on the sources of the passes, which have no way to reach a manager
// when they never name one.

namespace {

namespace fs = std::filesystem;

template <class T> concept complete_type = requires { sizeof(T); };

template <class C> concept has_textures_member = requires(C& c) { c.textures; };
template <class C> concept has_shaders_member = requires(C& c) { c.shaders; };
template <class C> concept has_geometry_member = requires(C& c) { c.geometry; };
template <class C> concept has_draw_data_member = requires(C& c) { c.draw_data; };
template <class C> concept has_skinning_member = requires(C& c) { c.skinning; };
template <class C> concept has_data_member = requires(C& c) { c.data; };
template <class C> concept hands_out_texture_manager = requires(C& c) { { c.get_texture_manager() } -> std::convertible_to<rnd::texture_manager&>; };

// Including these headers did not make a manager a complete type.
static_assert(!complete_type<rnd::texture_manager>, "rnd_render_system.h and the pass headers must not include the texture manager");
static_assert(!complete_type<rnd::ShaderManager>, "... nor the shader manager");
static_assert(!complete_type<rnd::geom_manager>, "... nor the geometry manager");

// A pass is given a render_context and nothing else.
static_assert(std::is_abstract_v<rnd::render_context>);
static_assert(std::is_same_v<decltype(&rnd::render_pass_interface::execute),
	void (rnd::render_pass_interface::*)(rnd::render_context&, rnd::driver::driver_interface&)>);
static_assert(!std::is_convertible_v<rnd::render_context&, rnd::frame_context&>);
static_assert(!std::is_base_of_v<rnd::frame_context, rnd::render_context>);

// The context has none of the data members the managers were reached through before, and the extract side's
// frame_context (which has the services extraction needs, but not a shader or geometry manager) is not what a pass gets.
static_assert(!has_textures_member<rnd::render_context>);
static_assert(!has_shaders_member<rnd::render_context>);
static_assert(!has_draw_data_member<rnd::render_context>);
static_assert(!has_skinning_member<rnd::render_context>);
static_assert(!has_data_member<rnd::render_context>);
static_assert(has_textures_member<rnd::frame_context>);
static_assert(!has_shaders_member<rnd::frame_context>);
static_assert(!has_geometry_member<rnd::frame_context>);
static_assert(!has_draw_data_member<rnd::frame_context>);
static_assert(!has_skinning_member<rnd::frame_context>);
// The render system keeps handing out the texture manager (the editor uses it); the others have no accessor.
static_assert(hands_out_texture_manager<rnd::render_system>);

std::string read_file(const fs::path& path)
{
	std::ifstream in(path, std::ios::binary);
	std::ostringstream text;
	text << in.rdbuf();
	return text.str();
}

// The source with // and /* */ comments blanked out, so a word in a comment is not taken for code.
std::string without_comments(const std::string& source)
{
	std::string result = source;
	for (std::size_t i = 0; i < source.size(); ++i) {
		const bool line = source.compare(i, 2, "//") == 0;
		if (!line && source.compare(i, 2, "/*") != 0) {
			continue;
		}
		const std::size_t end = line ? source.find('\n', i) : source.find("*/", i + 2);
		const std::size_t stop = end == std::string::npos ? source.size() : (line ? end : end + 2);
		for (std::size_t j = i; j < stop; ++j) {
			if (result[j] != '\n') {
				result[j] = ' ';
			}
		}
		i = stop - 1;
	}
	return result;
}

std::vector<std::string> includes_of(const std::string& source)
{
	static const std::regex include_line(R"(^[ \t]*#[ \t]*include[ \t]*[<"]([^>"]+)[>"])");
	std::vector<std::string> result;
	std::istringstream in(source);
	std::string line;
	while (std::getline(in, line)) {
		std::smatch match;
		if (std::regex_search(line, match, include_line)) {
			result.push_back(match[1]);
		}
	}
	return result;
}

bool is_manager_header(const std::string& include)
{
	return include.ends_with("rnd_texture_manager.h") || include.ends_with("rnd_shader_manager.h") || include.ends_with("rnd_geom_manager.h");
}

}

BOOST_AUTO_TEST_SUITE(RndRenderContextTests)

BOOST_AUTO_TEST_CASE(ThePassesNeverNameAManagerOrTheFrameContext)
{
	const fs::path root{ SNK_RENDER_SRC_DIR };
	std::size_t checked = 0;
	std::vector<fs::path> files;
	for (const auto& entry : fs::directory_iterator(root / "passes")) {
		if (entry.is_regular_file() && (entry.path().extension() == ".cpp" || entry.path().extension() == ".h")) {
			files.push_back(entry.path());
		}
	}
	files.push_back(root / "rnd_pass_pipelines.h");
	for (const fs::path& file : files) {
		const std::string source = without_comments(read_file(file));
		BOOST_TEST_CONTEXT(file.filename().string()) {
			BOOST_REQUIRE(!source.empty());
			++checked;
			for (const char* name : { "texture_manager", "ShaderManager", "geom_manager", "frame_context", "skinning_manager", "per_draw_data", "material_groups" }) {
				BOOST_TEST(source.find(name) == std::string::npos, "mentions " << name);
			}
			for (const std::string& include : includes_of(source)) {
				BOOST_TEST(!is_manager_header(include), "includes " << include);
				BOOST_TEST(include.find("rnd_render_system.h") == std::string::npos, "includes " << include);
				BOOST_TEST(include.find("rnd_frame_context.h") == std::string::npos, "includes " << include);
			}
		}
	}
	BOOST_TEST(checked >= 14u);	// seven passes with their headers, and the pipelines they share
}

BOOST_AUTO_TEST_CASE(TheRenderSystemHeaderIncludesNoManagerAndTheOnesThatAreInternalAreNotInstalled)
{
	const fs::path root{ SNK_RENDER_SRC_DIR };
	for (const char* header : { "rnd_render_system.h", "rnd_frame_context.h", "rnd_render_pass_interface.h", "rnd_render_context.h" }) {
		BOOST_TEST_CONTEXT(header) {
			const std::string source = without_comments(read_file(root / "include" / "snk" / "render" / header));
			BOOST_REQUIRE(!source.empty());
			for (const std::string& include : includes_of(source)) {
				BOOST_TEST(!is_manager_header(include), "includes " << include);
				BOOST_TEST(include.find("rnd_per_draw_data.h") == std::string::npos, "includes " << include);
			}
		}
	}

	// Everything under include/ is installed with the SDK: the shader and geometry managers and the pass helpers are not there.
	BOOST_TEST(!fs::exists(root / "include" / "snk" / "render" / "shader" / "rnd_shader_manager.h"));
	BOOST_TEST(!fs::exists(root / "include" / "snk" / "render" / "geom" / "rnd_geom_manager.h"));
	BOOST_TEST(!fs::exists(root / "include" / "snk" / "render" / "rnd_pass_pipelines.h"));
	BOOST_TEST(fs::exists(root / "shader" / "rnd_shader_manager.h"));
	BOOST_TEST(fs::exists(root / "geom" / "rnd_geom_manager.h"));
	BOOST_TEST(fs::exists(root / "rnd_pass_pipelines.h"));
}

BOOST_AUTO_TEST_SUITE_END()
