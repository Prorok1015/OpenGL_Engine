#include <boost/test/unit_test.hpp>
#include <snk/project/prj_project.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <string>
#include <system_error>

namespace {
	namespace fs = std::filesystem;

	constexpr std::string_view k_manifest = R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0",
		"start_scene": "res://levels/main.desc", "content_roots": ["res", "shared"] })";
}

BOOST_AUTO_TEST_SUITE(PrjProjectTests)

BOOST_AUTO_TEST_CASE(Open_ResolvesRootAndContentRoots)
{
	test_util::temp_dir dir("prj_open");
	dir.write("game/project.snkproj", k_manifest);
	fs::create_directories(dir.path() / "game" / "res");
	fs::create_directories(dir.path() / "game" / "shared");

	for (const fs::path& given : { dir.path() / "game", dir.path() / "game" / "project.snkproj" }) {
		BOOST_TEST_INFO("path: " << given.string());
		const auto opened = prj::open_project(given);
		BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
		const auto& ctx = *opened.context;
		BOOST_TEST(ctx.root == fs::canonical(dir.path() / "game"));
		BOOST_TEST(ctx.manifest_file == ctx.root / "project.snkproj");
		BOOST_TEST(ctx.data.name == "Sandbox");
		BOOST_REQUIRE(ctx.content_roots.size() == 2u);
		BOOST_TEST(ctx.content_roots[0] == ctx.root / "res");
		BOOST_TEST(ctx.content_roots[1] == ctx.root / "shared");
		BOOST_TEST(ctx.cache_dir() == ctx.root / ".cache");
		BOOST_TEST(ctx.user_profile_dir() == ctx.root / ".cache" / "user_profile");
		BOOST_TEST(ctx.build_dir() == ctx.root / "build");
	}
}

BOOST_AUTO_TEST_CASE(Open_MissingManifestOrBadSchemaIsAnError)
{
	test_util::temp_dir dir("prj_open_bad");
	auto missing = prj::open_project(dir.path());
	BOOST_TEST(!missing.context.has_value());
	BOOST_TEST(missing.error.find("project.snkproj") != std::string::npos);

	dir.write("project.snkproj", R"({ "format_version": 1, "name": "Sandbox" })");
	auto bad = prj::open_project(dir.path());
	BOOST_TEST(!bad.context.has_value());
	BOOST_TEST(bad.error.find("$.engine_version") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Open_ContentRootMustExist)
{
	test_util::temp_dir dir("prj_open_noroot");
	dir.write("project.snkproj", k_manifest);
	fs::create_directories(dir.path() / "res");

	const auto opened = prj::open_project(dir.path());
	BOOST_TEST(!opened.context.has_value());
	BOOST_TEST(opened.error.find("shared") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Open_ContentRootSymlinkLeavingTheProjectIsRejected)
{
	test_util::temp_dir dir("prj_open_link");
	dir.write("game/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	fs::create_directories(dir.path() / "outside");

	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "game" / "res", ec);
	if (ec) {
		// Windows without the symlink privilege: nothing to check.
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}

	const auto opened = prj::open_project(dir.path() / "game");
	BOOST_TEST(!opened.context.has_value());
	BOOST_TEST(opened.error.find("outside the project") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
