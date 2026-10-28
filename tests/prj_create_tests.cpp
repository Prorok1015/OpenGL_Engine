#include <boost/test/unit_test.hpp>
#include <snk/project/prj_create.h>
#include <snk/project/prj_project.h>
#include <snk/project/prj_version.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

// File > New Project (EPIC-42 #158): a project made from a template folder.

namespace {
	namespace fs = std::filesystem;

	// The one template the editor ships (Editor/data/templates/projects/empty).
	const fs::path k_empty_template = fs::path{ SNK_EDITOR_DATA_DIR } / "templates" / "projects" / "empty";

	std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	std::size_t entries_in(const fs::path& folder)
	{
		std::size_t count = 0;
		for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder)) ++count;
		return count;
	}
}

BOOST_AUTO_TEST_SUITE(PrjCreateTests)

BOOST_AUTO_TEST_CASE(Create_FromTheShippedTemplate_OpensWithItsNameAndThisEngine)
{
	test_util::temp_dir dir("prj_create");

	const auto created = prj::create_project(k_empty_template, dir.path() / "games" / "my_game", "My Game");
	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);

	const auto opened = prj::open_project(created.manifest_file);
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	BOOST_TEST(opened.context->data.name == "My Game");
	BOOST_TEST((opened.context->data.engine == prj::current_engine_version()));
	BOOST_TEST(opened.context->data.start_scene == "res://levels/main.desc");
	BOOST_TEST(fs::is_regular_file(dir.path() / "games" / "my_game" / "res" / "levels" / "main.desc"));
	// Indented for people, not one long line.
	BOOST_TEST(read_text(created.manifest_file).find("\n    \"name\": \"My Game\"") != std::string::npos);
}

// The game module's CMake project comes along (#434); the module is optional, so the project opens before it is built.
BOOST_AUTO_TEST_CASE(Create_FromTheShippedTemplate_HasTheGameModuleProject)
{
	test_util::temp_dir dir("prj_create_code");

	const auto created = prj::create_project(k_empty_template, dir.path() / "game", "Game");
	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);

	for (const char* file : { "code/CMakeLists.txt", "code/CMakePresets.json", "code/src/game_module.cpp" }) {
		BOOST_TEST_INFO("file: " << file);
		BOOST_TEST(fs::is_regular_file(dir.path() / "game" / file));
	}
	BOOST_TEST(!fs::exists(dir.path() / "game" / "build"));

	const auto opened = prj::open_project(created.manifest_file);
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	const auto& modules = opened.context->data.modules;
	BOOST_REQUIRE(modules.size() == 1u);
	BOOST_TEST(modules[0].name == "game");
	BOOST_TEST((modules[0].kind == prj::module_kind::runtime));
	BOOST_TEST(modules[0].optional);
	// The name the CMake project builds the module under is the manifest's.
	BOOST_TEST(read_text(dir.path() / "game" / "code" / "CMakeLists.txt").find("set(SNK_GAME_MODULE game)") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Create_IntoAnEmptyFolder_FillsIt)
{
	test_util::temp_dir dir("prj_create_empty");
	fs::create_directories(dir.path() / "picked");

	const auto created = prj::create_project(k_empty_template, dir.path() / "picked", "Picked");

	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
	BOOST_TEST(fs::is_regular_file(dir.path() / "picked" / "project.snkproj"));
	BOOST_TEST(entries_in(dir.path()) == 1u); // no staging folder left next to it
}

BOOST_AUTO_TEST_CASE(Create_OverSomethingThatIsNotAnEmptyFolder_IsRefusedAndTouchesNothing)
{
	test_util::temp_dir dir("prj_create_taken");
	dir.write("taken/keep.txt", "keep");
	dir.write("file", "a file");

	for (const char* target : { "taken", "file" }) {
		BOOST_TEST_INFO("target: " << target);
		const auto created = prj::create_project(k_empty_template, dir.path() / target, "Taken");
		BOOST_TEST(!created.error.empty());
		BOOST_TEST(created.manifest_file.empty());
	}
	BOOST_TEST(read_text(dir.path() / "taken" / "keep.txt") == "keep");
	BOOST_TEST(!fs::exists(dir.path() / "taken" / "project.snkproj"));
	BOOST_TEST(read_text(dir.path() / "file") == "a file");
	BOOST_TEST(entries_in(dir.path()) == 2u);
}

BOOST_AUTO_TEST_CASE(Create_WithABadName_IsRefused)
{
	test_util::temp_dir dir("prj_create_name");

	for (const std::string_view name : { std::string_view{}, std::string_view{ "a\nb" } }) {
		const auto created = prj::create_project(k_empty_template, dir.path() / "game", name);
		BOOST_TEST(!created.error.empty());
	}
	BOOST_TEST(!fs::exists(dir.path() / "game"));
}

BOOST_AUTO_TEST_CASE(Create_FromAFolderWithoutAManifest_IsRefusedAndLeavesNothing)
{
	test_util::temp_dir dir("prj_create_template");
	dir.write("not_a_template/readme.txt", "x");

	const auto created = prj::create_project(dir.path() / "not_a_template", dir.path() / "game", "Game");

	BOOST_TEST(created.error.find("template") != std::string::npos);
	BOOST_TEST(!fs::exists(dir.path() / "game"));
	BOOST_TEST(entries_in(dir.path()) == 1u);
}

BOOST_AUTO_TEST_CASE(Create_InsideTheTemplate_IsRefusedAndLeavesItAsItWas)
{
	test_util::temp_dir dir("prj_create_inside");
	fs::copy(k_empty_template, dir.path() / "tpl", fs::copy_options::recursive);
	const std::size_t before = entries_in(dir.path() / "tpl");

	for (const char* target : { "tpl/game", "tpl/res/game" }) {
		BOOST_TEST_INFO("target: " << target);
		const auto created = prj::create_project(dir.path() / "tpl", dir.path() / target, "Game");
		BOOST_TEST(created.error.find("template") != std::string::npos);
	}
	BOOST_TEST(entries_in(dir.path() / "tpl") == before);
	BOOST_TEST(!fs::exists(dir.path() / "tpl" / "game"));
	BOOST_TEST(!fs::exists(dir.path() / "tpl" / "res" / "game"));
}

BOOST_AUTO_TEST_SUITE_END()
