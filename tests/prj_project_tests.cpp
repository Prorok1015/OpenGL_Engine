#include <boost/test/unit_test.hpp>
#include <snk/project/prj_project.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifndef SNK_TEST_PROJECT_DIR
#error "SNK_TEST_PROJECT_DIR must point at projects/sandbox"
#endif

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
		BOOST_TEST(ctx.snk_dir() == ctx.root / ".snk");
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

BOOST_AUTO_TEST_CASE(Verify_CatchesAContentRootReplacedAfterOpening)
{
	test_util::temp_dir dir("prj_verify_later");
	dir.write("game/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	fs::create_directories(dir.path() / "game" / "res");
	fs::create_directories(dir.path() / "outside");

	auto opened = prj::open_project(dir.path() / "game");
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	BOOST_TEST(!prj::verify_project_mounts(*opened.context).has_value());

	fs::remove_all(dir.path() / "game" / "res");
	const auto removed = prj::verify_project_mounts(*opened.context);
	BOOST_REQUIRE(removed.has_value());
	BOOST_TEST(removed->find("not a directory") != std::string::npos);

	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "game" / "res", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto escaped = prj::verify_project_mounts(*opened.context);
	BOOST_REQUIRE(escaped.has_value());
	BOOST_TEST(escaped->find("outside the project") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Open_ComparesTheEngineVersion)
{
	test_util::temp_dir dir("prj_open_engine");
	fs::create_directories(dir.path() / "res");

	dir.write("project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": ")"
		+ prj::to_string(prj::current_engine_version()) + R"(" })");
	const auto same = prj::open_project(dir.path());
	BOOST_REQUIRE_MESSAGE(same.context.has_value(), same.error);
	BOOST_TEST((same.context->engine_match == prj::version_match::exact));

	dir.write("project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "999.0.0" })");
	const auto newer = prj::open_project(dir.path());
	BOOST_REQUIRE_MESSAGE(newer.context.has_value(), newer.error);
	BOOST_TEST((newer.context->engine_match == prj::version_match::project_newer));
}

BOOST_AUTO_TEST_CASE(HeldProject_ExitsWithAnErrorUnlessTheUserWentOn)
{
	prj::held_project held;
	BOOST_TEST(prj::exit_code_for(held) == prj::k_exit_engine_version);
	held.answer = prj::held_project::decision::quit;
	BOOST_TEST(prj::exit_code_for(held) == prj::k_exit_engine_version);
	held.answer = prj::held_project::decision::continue_without_project;
	BOOST_TEST(prj::exit_code_for(held) == 0);
	BOOST_TEST(prj::k_exit_engine_version != prj::k_exit_project_error);
}

BOOST_AUTO_TEST_CASE(Verify_RefusesOverlappingContentRoots)
{
	test_util::temp_dir dir("prj_overlap");
	dir.write("project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0",
		"content_roots": ["res", "res/sub"] })");
	fs::create_directories(dir.path() / "res" / "sub");

	const auto opened = prj::open_project(dir.path());
	BOOST_TEST(!opened.context.has_value());
	BOOST_TEST(opened.error.find("overlap") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Verify_RefusesContentRootsOverTheEngineBase)
{
	test_util::temp_dir dir("prj_engine_overlap");
	dir.write("project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	fs::create_directories(dir.path() / "res" / "engine_inside");
	const auto opened = prj::open_project(dir.path());
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);

	BOOST_TEST(!prj::verify_project_mounts(*opened.context, dir.path() / "elsewhere").has_value());
	for (const fs::path& base : { dir.path() / "res", dir.path() / "res" / "engine_inside", dir.path() }) {
		BOOST_TEST_INFO("engine base: " << base.string());
		const auto problem = prj::verify_project_mounts(*opened.context, base);
		BOOST_REQUIRE(problem.has_value());
		BOOST_TEST(problem->find("engine resources") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(Verify_RefusesSymlinkedWritableFolders)
{
	test_util::temp_dir dir("prj_cache_link");
	dir.write("game/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
	fs::create_directories(dir.path() / "game" / "res");
	fs::create_directories(dir.path() / "outside");

	std::error_code ec;
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "game" / ".cache", ec);
	if (ec) {
		BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
		return;
	}
	const auto cache_link = prj::open_project(dir.path() / "game");
	BOOST_TEST(!cache_link.context.has_value());
	BOOST_TEST(cache_link.error.find("symlink") != std::string::npos);

	// A real .cache with memory as the link.
	fs::remove(dir.path() / "game" / ".cache");
	fs::create_directories(dir.path() / "game" / ".cache");
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "game" / ".cache" / "memory", ec);
	BOOST_REQUIRE(!ec);
	const auto memory_link = prj::open_project(dir.path() / "game");
	BOOST_TEST(!memory_link.context.has_value());
	BOOST_TEST(memory_link.error.find("symlink") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Verify_RefusesContentRootsOverPrivateFolders)
{
	for (const std::string_view root : { ".snk", ".cache", ".cache/user_profile", ".cache/memory" }) {
		BOOST_TEST_INFO("content root: " << root);
		test_util::temp_dir dir("prj_private_root");
		dir.write("project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0", "content_roots": [")"
			+ std::string{ root } + R"("] })");
		fs::create_directories(dir.path() / root);

		const auto opened = prj::open_project(dir.path());
		BOOST_TEST(!opened.context.has_value());
		BOOST_TEST(opened.error.find("private folder") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(Verify_RefusesASymlinkedUserProfileAndSnk)
{
	for (const std::string_view link : { ".cache/user_profile", ".snk" }) {
		BOOST_TEST_INFO("link: " << link);
		test_util::temp_dir dir("prj_private_link");
		dir.write("game/project.snkproj", R"({ "format_version": 1, "name": "Sandbox", "engine_version": "0.2.0" })");
		fs::create_directories(dir.path() / "game" / "res");
		fs::create_directories(dir.path() / "game" / ".cache");
		fs::create_directories(dir.path() / "outside");

		std::error_code ec;
		fs::create_directory_symlink(dir.path() / "outside", dir.path() / "game" / link, ec);
		if (ec) {
			BOOST_TEST_MESSAGE("symlinks unavailable: " << ec.message());
			return;
		}
		const auto opened = prj::open_project(dir.path() / "game");
		BOOST_TEST(!opened.context.has_value());
		BOOST_TEST(opened.error.find("symlink") != std::string::npos);
	}
}

BOOST_AUTO_TEST_CASE(RefuseWithoutAsking_OnlyAHeldProjectInANonInteractiveRun)
{
	using prj::version_action;
	// -n never asks: what would be a question is refused, never opened.
	BOOST_TEST(prj::refuse_without_asking(version_action::ask_open_or_quit, true));
	BOOST_TEST(prj::refuse_without_asking(version_action::refuse, true));
	BOOST_TEST(!prj::refuse_without_asking(version_action::open, true));
	BOOST_TEST(!prj::refuse_without_asking(version_action::open_with_notice, true));
	// Someone can answer: the project is held and the dialog asks.
	for (const auto action : { version_action::open, version_action::open_with_notice, version_action::ask_open_or_quit,
			version_action::refuse }) {
		BOOST_TEST(!prj::refuse_without_asking(action, false));
	}
}

// -n on an older project: refused like a newer one, unless --allow-engine-version-mismatch opened it already.
BOOST_AUTO_TEST_CASE(RefuseWithoutAsking_AnOlderProjectOpensOnlyWithTheMismatchFlag)
{
	const auto older = prj::version_match::project_older;
	BOOST_TEST(prj::refuse_without_asking(prj::version_action_for(older, false), true));
	BOOST_TEST(!prj::refuse_without_asking(prj::version_action_for(older, true), true));
	BOOST_TEST(prj::refuse_without_asking(prj::version_action_for(prj::version_match::project_newer, true), true));
}

BOOST_AUTO_TEST_CASE(EngineVersionError_IsAProjectError)
{
	try {
		throw prj::engine_version_error("newer");
	} catch (const prj::project_error& e) {
		BOOST_TEST(std::string{ e.what() } == "newer");
		BOOST_TEST(dynamic_cast<const prj::engine_version_error*>(&e) != nullptr);
		return;
	}
	BOOST_FAIL("engine_version_error was not caught as a project_error");
}

// projects/sandbox is what CI and the release zip open (EPIC-42 #157). Raising SNK_SDK_VERSION past it would make
// every smoke run exit with 3 (they pass -n): bump its engine_version together with the SDK version.
BOOST_AUTO_TEST_CASE(TestProject_OpensWithThisEngineWithoutAsking)
{
	const auto opened = prj::open_project(SNK_TEST_PROJECT_DIR);
	BOOST_REQUIRE_MESSAGE(opened.context.has_value(), opened.error);
	const auto action = prj::version_action_for(opened.context->engine_match, false);
	BOOST_TEST((action == prj::version_action::open || action == prj::version_action::open_with_notice));

	const std::string_view start = opened.context->data.start_scene;
	BOOST_REQUIRE(start.starts_with("res://"));
	BOOST_TEST(fs::is_regular_file(opened.context->content_roots.front() / start.substr(6)));
}

BOOST_AUTO_TEST_SUITE_END()
