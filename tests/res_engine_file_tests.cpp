#include <boost/test/unit_test.hpp>
#include <snk/gui/gui_gui_service_init.h>
#include <snk/render/shader/rnd_shader_includes.h>
#include <snk/resource/res_system.h>
#include <snk/resource/resources/res_resource_binary.h>
#include <snk/resource/resources/res_resource_text.h>
#include "test_manual_tasks.hpp"
#include "test_temp_dir.hpp"
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// The files the engine loads by a fixed path (#929): from engine:// when it is mounted, so a project file at the same
// res:// path never reaches the font parser or the shader compiler in their place.

namespace {

namespace fs = std::filesystem;

std::string text_of(const std::vector<std::byte>& bytes)
{
	return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
}

struct engine_and_project
{
	test_util::temp_dir dir{ "res_engine_file" };
	fs::path engine_dir = dir.path() / "engine";
	fs::path project = dir.path() / "project" / "res";
	fs::path cache = dir.path() / "project" / ".cache";
	ds::task_system tasks{ test_util::manual_tasks() };

	engine_and_project()
	{
		dir.write("engine/fonts/NotoSans-Regular.ttf", "engine-font");
		dir.write("project/res/fonts/NotoSans-Regular.ttf", "project-font");
		dir.write("engine/shaders/pass.frag", "#version 450 core\n#include \"lib.glsl\"\n#include \"../../outside.glsl\"\nvoid main() {}\n");
		dir.write("engine/shaders/lib.glsl", "// engine-lib\n");
		dir.write("project/res/shaders/pass.frag", "// project-pass\n");
		dir.write("project/res/shaders/lib.glsl", "// project-lib\n");
		// Two levels above engine/shaders: outside the engine's root.
		dir.write("outside.glsl", "// outside\n");
		fs::create_directories(cache / "memory");
		fs::create_directories(cache / "user_profile");
	}

	res::mount_table table(const fs::path& engine_base) const
	{
		return res::resource_system::project_mount_table(engine_base, { project }, cache, cache / "user_profile");
	}
};

}

BOOST_FIXTURE_TEST_SUITE(ResEngineFileTests, engine_and_project)

BOOST_AUTO_TEST_CASE(WithEngineMounted_AnEngineFileIsTheEngines_NotTheProjectsAtTheSamePath)
{
	res::resource_system sys(tasks, table(engine_dir));

	const res::tag font = sys.engine_file("fonts/NotoSans-Regular.ttf");
	BOOST_TEST(font.view() == "engine://fonts/NotoSans-Regular.ttf");
	const auto engine_font = sys.require_sync<res::binary_resource>(font).get_sync();
	BOOST_REQUIRE(engine_font);
	BOOST_TEST(text_of(engine_font->bytes()) == "engine-font");
	// res:// at the same path is the project's file: what the engine read before.
	const auto project_font = sys.require_sync<res::binary_resource>(res::tag{ "res://fonts/NotoSans-Regular.ttf" }).get_sync();
	BOOST_REQUIRE(project_font);
	BOOST_TEST(text_of(project_font->bytes()) == "project-font");
}

BOOST_AUTO_TEST_CASE(WithoutEngineMounted_AnEngineFileIsRes)
{
	res::resource_system sys(tasks, table({}));
	BOOST_TEST(sys.engine_file("shaders/composition.frag").view() == "res://shaders/composition.frag");
}

BOOST_AUTO_TEST_CASE(TheUiFontIsReadFromTheEngine)
{
	res::resource_system sys(tasks, table(engine_dir));
	BOOST_TEST(text_of(engine::gui::load_ui_font_bytes(sys)) == "engine-font");
}

BOOST_AUTO_TEST_CASE(TheIncludesOfAnEngineShaderStayInTheEngine)
{
	res::resource_system sys(tasks, table(engine_dir));
	const res::tag origin = sys.engine_file("shaders/pass.frag");
	const auto source = sys.require_sync<res::text_resource>(origin).get_sync();
	BOOST_REQUIRE(source);
	std::string code = source->str();
	std::vector<res::tag> files;

	const std::string errors = rnd::expand_includes(code, origin, sys, &files);

	BOOST_TEST(code.find("engine-lib") != std::string::npos);
	BOOST_TEST(code.find("project-lib") == std::string::npos);
	// "../../outside.glsl" stays in engine:// and leaves its root: not read.
	BOOST_TEST(code.find("// outside") == std::string::npos);
	BOOST_TEST(errors.find("cannot read include \"../../outside.glsl\"") != std::string::npos, errors);
	BOOST_REQUIRE(files.size() == 2u);
	for (const res::tag& file : files) {
		BOOST_TEST(file.protocol() == "engine", file.view());
	}
}

BOOST_AUTO_TEST_SUITE_END()
