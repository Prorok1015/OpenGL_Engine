#include <boost/test/unit_test.hpp>
#include <snk/project/prj_manifest.h>
#include "test_temp_dir.hpp"
#include <algorithm>
#include <string>
#include <string_view>

namespace {
	constexpr std::string_view k_full_manifest = R"({
		"format_version": 1,
		"name": "Sandbox",
		"engine_version": "0.2.0",
		"start_scene": "res://levels/smoke_level.desc",
		"content_roots": ["res", "shared/res"],
		"modules": [
			{ "name": "game", "kind": "runtime" },
			{ "name": "game_editor", "kind": "editor", "optional": true }
		],
		"platforms": ["windows-x64"]
	})";

	constexpr std::string_view k_minimal_manifest = R"({ "format_version": 1, "name": "Mini", "engine_version": "0.2.0" })";

	// The minimal manifest with `field` set to `value` (raw JSON).
	std::string minimal_with(std::string_view field, std::string_view value)
	{
		return std::string{ R"({ "format_version": 1, "name": "Mini", "engine_version": "0.2.0", ")" } + std::string{ field } + "\": "
			+ std::string{ value } + " }";
	}

	void check_error(const prj::manifest_result& result, std::string_view json_path)
	{
		BOOST_TEST(!result.data.has_value());
		BOOST_REQUIRE(result.error.has_value());
		BOOST_TEST(result.error->json_path == json_path);
		BOOST_TEST(!result.error->reason.empty());
	}
}

BOOST_AUTO_TEST_SUITE(PrjManifestTests)

BOOST_AUTO_TEST_CASE(Parse_FullManifest)
{
	const auto result = prj::parse_manifest(k_full_manifest);
	BOOST_REQUIRE(result.data.has_value());
	BOOST_TEST(!result.error.has_value());
	BOOST_TEST(result.warnings.empty());

	const auto& m = *result.data;
	BOOST_TEST(m.format_version == 1);
	BOOST_TEST(m.name == "Sandbox");
	BOOST_TEST((m.engine == prj::engine_version{ 0, 2, 0 }));
	BOOST_TEST(m.start_scene == "res://levels/smoke_level.desc");
	BOOST_REQUIRE(m.content_roots.size() == 2u);
	BOOST_TEST(m.content_roots[0].generic_string() == "res");
	BOOST_TEST(m.content_roots[1].generic_string() == "shared/res");
	BOOST_REQUIRE(m.modules.size() == 2u);
	BOOST_TEST(m.modules[0].name == "game");
	BOOST_TEST((m.modules[0].kind == prj::module_kind::runtime));
	BOOST_TEST(!m.modules[0].optional);
	BOOST_TEST((m.modules[1].kind == prj::module_kind::editor));
	BOOST_TEST(m.modules[1].optional);
	BOOST_REQUIRE(m.platforms.size() == 1u);
	BOOST_TEST(m.platforms[0] == "windows-x64");
}

BOOST_AUTO_TEST_CASE(Parse_MinimalManifestGetsDefaults)
{
	const auto result = prj::parse_manifest(k_minimal_manifest);
	BOOST_REQUIRE(result.data.has_value());
	BOOST_TEST(result.data->start_scene.empty());
	BOOST_REQUIRE(result.data->content_roots.size() == 1u);
	BOOST_TEST(result.data->content_roots[0].generic_string() == "res");
	BOOST_TEST(result.data->modules.empty());
	BOOST_TEST(result.data->platforms.empty());
}

BOOST_AUTO_TEST_CASE(Parse_ModuleKindDefaultsToRuntime)
{
	const auto result = prj::parse_manifest(minimal_with("modules", R"([{ "name": "game" }])"));
	BOOST_REQUIRE(result.data.has_value());
	BOOST_REQUIRE(result.data->modules.size() == 1u);
	BOOST_TEST((result.data->modules[0].kind == prj::module_kind::runtime));
}

BOOST_AUTO_TEST_CASE(Parse_UnknownFieldsWarnButLoad)
{
	const auto result = prj::parse_manifest(
		R"({ "format_version": 1, "name": "Mini", "engine_version": "0.2.0", "future": 3,
			"modules": [{ "name": "game", "hot_reload": true }] })");
	BOOST_REQUIRE(result.data.has_value());
	BOOST_REQUIRE(result.warnings.size() == 2u);
	BOOST_TEST(result.warnings[0].find("$.future") != std::string::npos);
	BOOST_TEST(result.warnings[1].find("$.modules[0].hot_reload") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Parse_InvalidJsonIsAFileLevelError)
{
	for (const std::string_view text : { "", "{", "not json", R"({ "name": "x", })",
			R"({ /* comment */ "format_version": 1, "name": "Mini", "engine_version": "0.2.0" })" }) {
		BOOST_TEST_INFO("text: " << text);
		check_error(prj::parse_manifest(text), "");
	}
}

BOOST_AUTO_TEST_CASE(Parse_RootMustBeAnObject)
{
	check_error(prj::parse_manifest("[]"), "$");
	check_error(prj::parse_manifest("42"), "$");
}

BOOST_AUTO_TEST_CASE(Parse_MissingRequiredFields)
{
	check_error(prj::parse_manifest(R"({ "name": "Mini", "engine_version": "0.2.0" })"), "$.format_version");
	check_error(prj::parse_manifest(R"({ "format_version": 1, "engine_version": "0.2.0" })"), "$.name");
	check_error(prj::parse_manifest(R"({ "format_version": 1, "name": "Mini" })"), "$.engine_version");
	check_error(prj::parse_manifest(minimal_with("modules", R"([{ "kind": "runtime" }])")), "$.modules[0].name");
}

BOOST_AUTO_TEST_CASE(Parse_WrongTypes)
{
	check_error(prj::parse_manifest(R"({ "format_version": "1", "name": "Mini", "engine_version": "0.2.0" })"), "$.format_version");
	check_error(prj::parse_manifest(R"({ "format_version": 1.0, "name": "Mini", "engine_version": "0.2.0" })"), "$.format_version");
	check_error(prj::parse_manifest(R"({ "format_version": 1, "name": 7, "engine_version": "0.2.0" })"), "$.name");
	check_error(prj::parse_manifest(R"({ "format_version": 1, "name": "", "engine_version": "0.2.0" })"), "$.name");
	check_error(prj::parse_manifest(R"({ "format_version": 1, "name": "Mini", "engine_version": "0.2" })"), "$.engine_version");
	check_error(prj::parse_manifest(minimal_with("start_scene", "1")), "$.start_scene");
	check_error(prj::parse_manifest(minimal_with("content_roots", R"("res")")), "$.content_roots");
	check_error(prj::parse_manifest(minimal_with("content_roots", "[1]")), "$.content_roots[0]");
	check_error(prj::parse_manifest(minimal_with("modules", R"({})")), "$.modules");
	check_error(prj::parse_manifest(minimal_with("modules", R"(["game"])")), "$.modules[0]");
	check_error(prj::parse_manifest(minimal_with("modules", R"([{ "name": "game", "kind": "server" }])")), "$.modules[0].kind");
	check_error(prj::parse_manifest(minimal_with("modules", R"([{ "name": "game", "optional": "yes" }])")), "$.modules[0].optional");
	check_error(prj::parse_manifest(minimal_with("platforms", R"([""])")), "$.platforms[0]");
}

BOOST_AUTO_TEST_CASE(Parse_FormatVersionFromTheFutureIsRejected)
{
	const auto result = prj::parse_manifest(R"({ "format_version": 2, "name": "Mini", "engine_version": "0.2.0" })");
	check_error(result, "$.format_version");
	BOOST_TEST(result.error->reason.find("newer") != std::string::npos);
	check_error(prj::parse_manifest(R"({ "format_version": 0, "name": "Mini", "engine_version": "0.2.0" })"), "$.format_version");
	check_error(prj::parse_manifest(R"({ "format_version": 99999999999, "name": "Mini", "engine_version": "0.2.0" })"), "$.format_version");
}

BOOST_AUTO_TEST_CASE(Parse_ContentRootsMustStayInsideTheProject)
{
	for (const std::string_view root : { R"("")", R"(".")", R"("..")", R"("../engine/res")", R"("res/../../x")", R"("/abs/res")",
			R"("\\server\\share")", R"("C:/res")", R"("res:x")", R"("res//x")", R"("./res")" }) {
		BOOST_TEST_INFO("root: " << root);
		check_error(prj::parse_manifest(minimal_with("content_roots", "[" + std::string{ root } + "]")), "$.content_roots[0]");
	}
	check_error(prj::parse_manifest(minimal_with("content_roots", "[]")), "$.content_roots");
}

BOOST_AUTO_TEST_CASE(Parse_ContentRootsAcceptNestedAndTrailingSeparator)
{
	const auto result = prj::parse_manifest(minimal_with("content_roots", R"(["res/", "content\\shared"])"));
	BOOST_REQUIRE(result.data.has_value());
	BOOST_TEST(result.data->content_roots.size() == 2u);
}

BOOST_AUTO_TEST_CASE(Parse_DuplicatesAreRejected)
{
	check_error(prj::parse_manifest(minimal_with("content_roots", R"(["res", "res/"])")), "$.content_roots[1]");
	check_error(prj::parse_manifest(minimal_with("modules", R"([{ "name": "game" }, { "name": "game", "kind": "editor" }])")),
		"$.modules[1].name");
}

BOOST_AUTO_TEST_CASE(Parse_StartSceneMustBeAResTagInsideTheProject)
{
	for (const std::string_view scene : { R"("levels/a.desc")", R"("memory://a.desc")", R"("res://")", R"("res://../a.desc")",
			R"("res:///abs.desc")", R"("res://levels/")" }) {
		BOOST_TEST_INFO("scene: " << scene);
		check_error(prj::parse_manifest(minimal_with("start_scene", scene)), "$.start_scene");
	}
}

BOOST_AUTO_TEST_CASE(ToString_IncludesThePath)
{
	BOOST_TEST(prj::to_string(prj::manifest_error{ "$.name", "expected a string" }) == "$.name: expected a string");
	BOOST_TEST(prj::to_string(prj::manifest_error{ "", "not found" }) == "not found");
}

BOOST_AUTO_TEST_CASE(Load_FromFileAndFromDirectory)
{
	test_util::temp_dir dir("prj_manifest_load");
	const auto file = dir.write(std::string{ prj::k_manifest_file_name }, k_minimal_manifest);

	const auto by_file = prj::load_manifest(file);
	BOOST_REQUIRE(by_file.data.has_value());
	BOOST_TEST(by_file.data->name == "Mini");

	const auto by_dir = prj::load_manifest(dir.path());
	BOOST_REQUIRE(by_dir.data.has_value());
	BOOST_TEST(by_dir.data->name == "Mini");
}

BOOST_AUTO_TEST_CASE(Load_MissingManifestNamesThePath)
{
	test_util::temp_dir dir("prj_manifest_missing");

	const auto result = prj::load_manifest(dir.path());
	check_error(result, "");
	BOOST_TEST(result.error->reason.find(std::string{ prj::k_manifest_file_name }) != std::string::npos);

	check_error(prj::load_manifest(dir.path() / "nowhere" / "game.snkproj"), "");
}

BOOST_AUTO_TEST_CASE(Load_ReportsSchemaErrorsFromTheFile)
{
	test_util::temp_dir dir("prj_manifest_bad");
	dir.write(std::string{ prj::k_manifest_file_name }, R"({ "format_version": 1, "name": "Mini" })");
	check_error(prj::load_manifest(dir.path()), "$.engine_version");
}

BOOST_AUTO_TEST_SUITE_END()
