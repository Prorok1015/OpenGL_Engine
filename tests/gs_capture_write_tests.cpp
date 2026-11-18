#include <boost/test/unit_test.hpp>

#include <snk/game/gs_capture_on_settled.h>

#include "test_temp_dir.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
	std::string read_all(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), {});
	}
}

// #1161: capture/on_settled is an output the launcher names, not project data: written with a rename, never inside the
// project, never through a link at the file.
BOOST_AUTO_TEST_SUITE(GsCaptureWriteTests)

BOOST_AUTO_TEST_CASE(ACapture_IsWrittenOutsideTheProject_AndReplacesAnOldOne)
{
	const test_util::temp_dir dir("gs_capture_write_outside");
	const auto project = dir.path() / "project";
	std::filesystem::create_directories(project);
	const auto file = dir.path() / "out" / "scene.png";
	std::filesystem::create_directories(file.parent_path());

	BOOST_TEST(gs::write_capture(file, "first", project).empty());
	BOOST_TEST(read_all(file) == "first");
	BOOST_TEST(gs::write_capture(file, "second", project).empty());
	BOOST_TEST(read_all(file) == "second");
	BOOST_TEST(gs::write_capture(dir.path() / "no_project.png", "x", {}).empty());
}

BOOST_AUTO_TEST_CASE(ACapture_InsideTheProject_IsRefused_AndNothingIsWritten)
{
	const test_util::temp_dir dir("gs_capture_write_inside");
	const auto project = dir.path() / "project";
	std::filesystem::create_directories(project / "res");

	const std::string refused = gs::write_capture(project / "res" / "scene.png", "png", project);
	BOOST_TEST(refused.find("inside the project") != std::string::npos, refused);
	BOOST_TEST(!std::filesystem::exists(project / "res" / "scene.png"));
	// A path that only spells its way into the project is the same file.
	const std::string dotted = gs::write_capture(dir.path() / "out" / ".." / "project" / "x.png", "png", project);
	BOOST_TEST(dotted.find("inside the project") != std::string::npos, dotted);
	BOOST_TEST(!std::filesystem::exists(project / "x.png"));
	// A sibling folder whose name starts like the project's is outside it.
	std::filesystem::create_directories(dir.path() / "project_out");
	BOOST_TEST(gs::write_capture(dir.path() / "project_out" / "x.png", "png", project).empty());
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(ALinkAtTheCapture_IsRefused_AndItsTargetStays)
{
	const test_util::temp_dir dir("gs_capture_write_link");
	const auto target = dir.write("target.txt", "keep");
	const auto link = dir.path() / "scene.png";
	std::filesystem::create_symlink(target, link);

	BOOST_TEST(!gs::write_capture(link, "png", {}).empty());
	BOOST_TEST(read_all(target) == "keep");
}
#endif

BOOST_AUTO_TEST_SUITE_END()
