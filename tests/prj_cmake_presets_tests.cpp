#include <boost/test/unit_test.hpp>
#include <snk/project/prj_cmake_presets.h>
#include <snk/project/prj_json_text.h>
#include "test_temp_dir.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

// The game module's CMake presets (#444): a read-only view of CMakePresets.json and the editor's edits of
// CMakeUserPresets.json, which never lose what a person wrote there.

namespace {
	namespace fs = std::filesystem;
	namespace json = boost::json;

	// What the shipped template has (Editor/data/templates/projects/empty/code/CMakePresets.json).
	constexpr std::string_view k_main = R"({
    "version": 3,
    "configurePresets": [
        {
            "name": "base",
            "hidden": true,
            "generator": "Ninja",
            "binaryDir": "${sourceDir}/../build/${presetName}",
            "cacheVariables": { "CMAKE_EXPORT_COMPILE_COMMANDS": "ON" }
        },
        {
            "name": "debuggame",
            "displayName": "DebugGame",
            "inherits": "base",
            "cacheVariables": { "CMAKE_BUILD_TYPE": "DebugGame" }
        },
        {
            "name": "release",
            "displayName": "Release",
            "inherits": "base",
            "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" }
        }
    ],
    "buildPresets": [
        { "name": "debuggame", "configurePreset": "debuggame" },
        { "name": "release", "configurePreset": "release", "jobs": 4 }
    ]
}
)";

	// Hand-written: a comment, a vendor field, a preset of the person's own and extra keys in the editor's preset.
	constexpr std::string_view k_hand_user = R"({
  "$comment": "my presets",
  "version": 3,
  "vendor": { "example.com/tool": { "x": [1, 2] } },
  "configurePresets": [
    { "name": "mine", "inherits": "release", "cacheVariables": { "FOO": "1" } },
    {
      "name": "user-debuggame",
      "inherits": "debuggame",
      "environment": { "CC": "clang" },
      "cacheVariables": { "SNK_X": { "type": "BOOL", "value": "OFF" }, "KEEP": "yes" }
    }
  ],
  "buildPresets": [
    { "name": "mine", "configurePreset": "mine" },
    { "name": "user-debuggame", "configurePreset": "user-debuggame", "targets": ["all"] }
  ]
}
)";

	prj::main_presets parse_main(std::string_view text)
	{
		auto parsed = prj::parse_cmake_presets(text);
		BOOST_REQUIRE_MESSAGE(parsed.presets.has_value(), parsed.error);
		return *parsed.presets;
	}

	std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	std::string text_of(const json::value& value)
	{
		const auto& text = value.as_string();
		return { text.data(), text.size() };
	}

	std::vector<std::string> keys_of(const json::value& value)
	{
		std::vector<std::string> keys;
		for (const auto& member : value.as_object()) {
			keys.emplace_back(member.key().data(), member.key().size());
		}
		return keys;
	}

	const json::object& preset_in(const json::value& doc, std::string_view array, std::string_view name)
	{
		for (const auto& item : doc.at(array).as_array()) {
			if (text_of(item.at("name")) == name) {
				return item.as_object();
			}
		}
		BOOST_FAIL("no preset " << name);
		throw 0;
	}

	std::size_t presets_named(const json::value& doc, std::string_view array, std::string_view name)
	{
		std::size_t count = 0;
		for (const auto& item : doc.at(array).as_array()) {
			count += text_of(item.at("name")) == name ? 1 : 0;
		}
		return count;
	}
}

BOOST_AUTO_TEST_SUITE(PrjCmakePresetsTests)

BOOST_AUTO_TEST_CASE(IndentedJson_IsTheManifestFormat)
{
	const json::value value = json::parse(R"({"a":1,"b":[true,null],"c":{},"d":[],"e":{"f":"g"}})");
	BOOST_TEST(prj::to_indented_json(value) ==
		"{\n    \"a\": 1,\n    \"b\": [\n        true,\n        null\n    ],\n    \"c\": {},\n    \"d\": [],\n"
		"    \"e\": {\n        \"f\": \"g\"\n    }\n}\n");
}

BOOST_AUTO_TEST_CASE(MainView_ResolvesInheritance)
{
	const auto main = parse_main(k_main);
	BOOST_TEST(main.version == 3);
	BOOST_REQUIRE(main.configure.size() == 3u);
	BOOST_TEST(main.configure[0].hidden);

	const auto* debuggame = main.find_configure("debuggame");
	BOOST_REQUIRE(debuggame);
	BOOST_TEST(!debuggame->hidden);
	BOOST_TEST(debuggame->display_name == "DebugGame");
	BOOST_TEST(debuggame->generator == "Ninja");
	BOOST_TEST(debuggame->binary_dir == "${sourceDir}/../build/${presetName}");
	BOOST_TEST((debuggame->inherits == std::vector<std::string>{ "base" }));
	BOOST_TEST(debuggame->cache_variables.size() == 2u);
	BOOST_TEST(debuggame->cache_variables.at("CMAKE_BUILD_TYPE") == "DebugGame");
	BOOST_TEST(debuggame->cache_variables.at("CMAKE_EXPORT_COMPILE_COMMANDS") == "ON");

	BOOST_REQUIRE(main.build.size() == 2u);
	BOOST_TEST(main.build[0].configure_preset == "debuggame");
	BOOST_TEST(!main.build[0].jobs.has_value());
	BOOST_TEST((main.build[1].jobs == 4));
}

BOOST_AUTO_TEST_CASE(MainView_InheritsArray_FirstParentWins_NullUnsets_ObjectFormIsItsValue)
{
	const auto main = parse_main(R"({ "version": 6, "configurePresets": [
		{ "name": "a", "hidden": true, "cacheVariables": { "X": "a", "GONE": "a", "T": { "type": "BOOL", "value": true } } },
		{ "name": "b", "hidden": true, "binaryDir": "out/b", "cacheVariables": { "X": "b", "Y": "b" } },
		{ "name": "both", "inherits": ["a", "b"], "cacheVariables": { "GONE": null } },
		{ "name": "loop1", "inherits": "loop2", "cacheVariables": { "L": "1" } },
		{ "name": "loop2", "inherits": "loop1" }
	] })");
	const auto* both = main.find_configure("both");
	BOOST_REQUIRE(both);
	BOOST_TEST(both->cache_variables.at("X") == "a");
	BOOST_TEST(both->cache_variables.at("Y") == "b");
	BOOST_TEST(both->cache_variables.at("T") == "TRUE");
	BOOST_TEST(!both->cache_variables.contains("GONE"));
	BOOST_TEST(both->binary_dir == "out/b");

	// A cycle ends; the presets on it still read.
	const auto* loop2 = main.find_configure("loop2");
	BOOST_REQUIRE(loop2);
	BOOST_TEST(loop2->cache_variables.at("L") == "1");
	BOOST_TEST(loop2->binary_dir.empty());
}

BOOST_AUTO_TEST_CASE(MainView_ParseError_NamesThePosition)
{
	const auto parsed = prj::parse_cmake_presets("{\n  \"version\": 3,\n  \"configurePresets\": [ x ]\n}");
	BOOST_TEST(!parsed.presets.has_value());
	BOOST_TEST(parsed.error.find("line 3") != std::string::npos, parsed.error);

	const auto no_version = prj::parse_cmake_presets(R"({ "configurePresets": [] })");
	BOOST_TEST(!no_version.presets.has_value());
	BOOST_TEST(!no_version.error.empty());
}

BOOST_AUTO_TEST_CASE(UserBinaryDir_IsTheParentsTree)
{
	const auto main = parse_main(k_main);
	BOOST_TEST(prj::user_binary_dir(main, "debuggame") == "${sourceDir}/../build/debuggame");
	BOOST_TEST(prj::user_binary_dir(main, "release") == "${sourceDir}/../build/release");

	const auto through_array = parse_main(R"({ "version": 3, "configurePresets": [
		{ "name": "plain", "hidden": true },
		{ "name": "tree", "hidden": true, "binaryDir": "${sourceDir}/out/${presetName}-${presetName}" },
		{ "name": "dev", "inherits": ["plain", "tree"] },
		{ "name": "bare" }
	] })");
	BOOST_TEST(prj::user_binary_dir(through_array, "dev") == "${sourceDir}/out/dev-dev");
	BOOST_TEST(prj::user_binary_dir(through_array, "bare") == "${sourceDir}/../build/bare");
}

BOOST_AUTO_TEST_CASE(Apply_ToNothing_MakesTheSkeleton)
{
	const auto main = parse_main(k_main);
	json::value doc;
	const auto problem = prj::apply_user_presets_edits(doc, main, { prj::ensure_user_preset{ "debuggame" } });
	BOOST_REQUIRE_MESSAGE(!problem, problem.value_or(""));

	BOOST_TEST((keys_of(doc) == std::vector<std::string>{ "version", "configurePresets", "buildPresets" }));
	BOOST_TEST((doc.at("version").as_int64() == 3));
	const auto& configure = preset_in(doc, "configurePresets", "user-debuggame");
	BOOST_TEST(text_of(configure.at("inherits")) == "debuggame");
	BOOST_TEST(text_of(configure.at("binaryDir")) == "${sourceDir}/../build/debuggame");
	BOOST_TEST(configure.at("cacheVariables").as_object().empty());
	const auto& build = preset_in(doc, "buildPresets", "user-debuggame");
	BOOST_TEST(text_of(build.at("configurePreset")) == "user-debuggame");
	BOOST_TEST(!build.contains("jobs"));
	BOOST_TEST(prj::has_user_preset(doc, "debuggame"));
	BOOST_TEST(!prj::has_user_preset(doc, "release"));
	BOOST_TEST(prj::user_preset_name("release") == "user-release");

	// Ensuring twice adds nothing.
	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main, { prj::ensure_user_preset{ "debuggame" } }));
	BOOST_TEST(presets_named(doc, "configurePresets", "user-debuggame") == 1u);
	BOOST_TEST(presets_named(doc, "buildPresets", "user-debuggame") == 1u);
}

BOOST_AUTO_TEST_CASE(Apply_UnknownConfiguration_IsAnError)
{
	const auto main = parse_main(k_main);
	json::value doc;
	BOOST_TEST(prj::apply_user_presets_edits(doc, main, { prj::set_define{ "shipping", "A", "1" } }).has_value());
	BOOST_TEST(prj::apply_user_presets_edits(doc, main, { prj::set_jobs{ "debuggame", 0 } }).has_value());
}

BOOST_AUTO_TEST_CASE(Apply_KeepsWhatThePersonWrote)
{
	const auto main = parse_main(k_main);
	const json::value original = json::parse(k_hand_user);
	json::value doc = original;
	const auto problem = prj::apply_user_presets_edits(doc, main, {
		prj::set_define{ "debuggame", "NEW", "1" },
		prj::set_jobs{ "debuggame", 6 },
		prj::set_define{ "release", "R", "on" },
	});
	BOOST_REQUIRE_MESSAGE(!problem, problem.value_or(""));

	// Order, $comment and the unknown field stay; the person's own presets are not touched.
	BOOST_TEST((keys_of(doc) == keys_of(original)));
	BOOST_TEST((doc.at("$comment") == original.at("$comment")));
	BOOST_TEST((doc.at("vendor") == original.at("vendor")));
	BOOST_TEST((preset_in(doc, "configurePresets", "mine") == preset_in(original, "configurePresets", "mine")));
	BOOST_TEST((preset_in(doc, "buildPresets", "mine") == preset_in(original, "buildPresets", "mine")));
	BOOST_TEST(text_of(doc.at("configurePresets").as_array()[0].at("name")) == "mine");

	const auto& configure = preset_in(doc, "configurePresets", "user-debuggame");
	BOOST_TEST((keys_of(configure) == std::vector<std::string>{ "name", "inherits", "environment", "cacheVariables" }));
	BOOST_TEST((configure.at("environment") == preset_in(original, "configurePresets", "user-debuggame").at("environment")));
	BOOST_TEST((keys_of(configure.at("cacheVariables")) == std::vector<std::string>{ "SNK_X", "KEEP", "NEW" }));
	const auto& build = preset_in(doc, "buildPresets", "user-debuggame");
	BOOST_TEST((build.at("targets") == preset_in(original, "buildPresets", "user-debuggame").at("targets")));
	BOOST_TEST((build.at("jobs").as_int64() == 6));

	// A new preset goes after the existing ones.
	BOOST_TEST(text_of(doc.at("configurePresets").as_array().back().at("name")) == "user-release");
	BOOST_TEST(prj::user_defines(doc, "release").at("R") == "on");
}

BOOST_AUTO_TEST_CASE(Apply_SetAndRemoveDefine)
{
	const auto main = parse_main(k_main);
	json::value doc;
	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main, {
		prj::set_define{ "debuggame", "A", "1" },
		prj::set_define{ "debuggame", "B", "2" },
		prj::set_define{ "debuggame", "A", "3" },
	}));
	auto defines = prj::user_defines(doc, "debuggame");
	BOOST_TEST(defines.size() == 2u);
	BOOST_TEST(defines.at("A") == "3");
	BOOST_TEST(defines.at("B") == "2");

	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main, {
		prj::remove_define{ "debuggame", "A" },
		prj::remove_define{ "debuggame", "MISSING" },
		prj::remove_define{ "release", "A" },
	}));
	defines = prj::user_defines(doc, "debuggame");
	BOOST_TEST(defines.size() == 1u);
	BOOST_TEST(defines.contains("B"));
	// Removing from a preset that does not exist does not create it.
	BOOST_TEST(!prj::has_user_preset(doc, "release"));
}

BOOST_AUTO_TEST_CASE(Apply_ObjectFormDefine_KeepsItsType)
{
	const auto main = parse_main(k_main);
	json::value doc = json::parse(k_hand_user);
	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main, { prj::set_define{ "debuggame", "SNK_X", "ON" } }));
	const auto& entry = preset_in(doc, "configurePresets", "user-debuggame").at("cacheVariables").at("SNK_X");
	BOOST_REQUIRE(entry.is_object());
	BOOST_TEST(text_of(entry.at("type")) == "BOOL");
	BOOST_TEST(text_of(entry.at("value")) == "ON");
	BOOST_TEST(prj::user_defines(doc, "debuggame").at("SNK_X") == "ON");
}

BOOST_AUTO_TEST_CASE(Apply_SetAndRemoveJobs)
{
	const auto main = parse_main(k_main);
	json::value doc;
	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main, { prj::set_jobs{ "release", 12 } }));
	BOOST_TEST((prj::user_jobs(doc, "release") == 12));
	BOOST_TEST(prj::has_user_preset(doc, "release"));

	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main, { prj::set_jobs{ "release", std::nullopt } }));
	BOOST_TEST(!prj::user_jobs(doc, "release").has_value());
	BOOST_TEST(!preset_in(doc, "buildPresets", "user-release").contains("jobs"));
}

BOOST_AUTO_TEST_CASE(Apply_VersionFollowsTheMainFile)
{
	const auto main6 = parse_main(R"({ "version": 6, "configurePresets": [ { "name": "debuggame" } ] })");
	json::value doc = json::parse(R"({ "version": 3, "configurePresets": [] })");
	BOOST_REQUIRE(!prj::apply_user_presets_edits(doc, main6, { prj::ensure_user_preset{ "debuggame" } }));
	BOOST_TEST((doc.at("version").as_int64() == 6));

	// Never below 3, never lowered.
	const auto main2 = parse_main(R"({ "version": 2, "configurePresets": [ { "name": "debuggame" } ] })");
	json::value fresh;
	BOOST_REQUIRE(!prj::apply_user_presets_edits(fresh, main2, { prj::ensure_user_preset{ "debuggame" } }));
	BOOST_TEST(fresh.at("version").as_int64() == 3);
	json::value newer = json::parse(R"({ "version": 8 })");
	BOOST_REQUIRE(!prj::apply_user_presets_edits(newer, main6, { prj::ensure_user_preset{ "debuggame" } }));
	BOOST_TEST(newer.at("version").as_int64() == 8);
}

BOOST_AUTO_TEST_CASE(Save_CreatesTheUserFile)
{
	test_util::temp_dir dir("prj_presets_create");
	dir.write("CMakePresets.json", k_main);

	// No edits, no file.
	auto saved = prj::save_user_presets(dir.path(), {});
	BOOST_TEST(saved.error.empty(), saved.error);
	BOOST_TEST(!saved.written);
	BOOST_TEST(!fs::exists(dir.path() / "CMakeUserPresets.json"));

	saved = prj::save_user_presets(dir.path(), { prj::set_define{ "debuggame", "A", "1" } });
	BOOST_REQUIRE_MESSAGE(saved.error.empty(), saved.error);
	BOOST_TEST(saved.written);
	const auto read = prj::read_user_presets(dir.path());
	BOOST_REQUIRE_MESSAGE(read.error.empty(), read.error);
	BOOST_TEST(prj::user_defines(read.doc, "debuggame").at("A") == "1");
	BOOST_TEST(read_text(dir.path() / "CMakeUserPresets.json") == prj::to_indented_json(read.doc));
	// The main file is never written.
	BOOST_TEST(read_text(dir.path() / "CMakePresets.json") == k_main);
	// No temporary file is left behind.
	std::size_t entries = 0;
	for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir.path())) ++entries;
	BOOST_TEST(entries == 2u);
}

BOOST_AUTO_TEST_CASE(Save_NothingChanged_DoesNotRewrite)
{
	test_util::temp_dir dir("prj_presets_unchanged");
	dir.write("CMakePresets.json", k_main);
	const fs::path user = dir.write("CMakeUserPresets.json", k_hand_user);

	const auto saved = prj::save_user_presets(dir.path(), {
		prj::ensure_user_preset{ "debuggame" },
		prj::set_define{ "debuggame", "KEEP", "yes" },
		prj::remove_define{ "debuggame", "MISSING" },
	});
	BOOST_TEST(saved.error.empty(), saved.error);
	BOOST_TEST(!saved.written);
	// The person's formatting stays too.
	BOOST_TEST(read_text(user) == k_hand_user);
}

BOOST_AUTO_TEST_CASE(Save_BrokenUserFile_IsNotOverwritten)
{
	test_util::temp_dir dir("prj_presets_broken");
	dir.write("CMakePresets.json", k_main);
	constexpr std::string_view broken = "{ \"version\": 3,\n  \"configurePresets\": [ { \"name\": \"mine\", } ]\n}\n";
	const fs::path user = dir.write("CMakeUserPresets.json", broken);

	const auto saved = prj::save_user_presets(dir.path(), { prj::set_define{ "debuggame", "A", "1" } });
	BOOST_TEST(!saved.written);
	BOOST_TEST(saved.error.find("line 2") != std::string::npos, saved.error);
	BOOST_TEST(read_text(user) == broken);

	const auto read = prj::read_user_presets(dir.path());
	BOOST_TEST(!read.error.empty());
	BOOST_TEST(read.doc.is_null());
}

BOOST_AUTO_TEST_CASE(Save_BrokenOrMissingMainFile_IsAnError)
{
	test_util::temp_dir dir("prj_presets_no_main");
	auto saved = prj::save_user_presets(dir.path(), { prj::set_define{ "debuggame", "A", "1" } });
	BOOST_TEST(!saved.error.empty());
	BOOST_TEST(!saved.written);
	BOOST_TEST(!fs::exists(dir.path() / "CMakeUserPresets.json"));

	dir.write("CMakePresets.json", "{ \"version\": ");
	saved = prj::save_user_presets(dir.path(), { prj::set_define{ "debuggame", "A", "1" } });
	BOOST_TEST(!saved.error.empty());
	BOOST_TEST(!fs::exists(dir.path() / "CMakeUserPresets.json"));
}

// The editor read the file, then something else changed it before the save: the edits go onto the new content.
BOOST_AUTO_TEST_CASE(Save_FileChangedOnDisk_KeepsTheExternalChange)
{
	test_util::temp_dir dir("prj_presets_external");
	dir.write("CMakePresets.json", k_main);
	dir.write("CMakeUserPresets.json", k_hand_user);
	const auto seen_by_editor = prj::read_user_presets(dir.path());
	BOOST_REQUIRE(seen_by_editor.error.empty());
	const std::vector<prj::user_presets_edit> edits{
		prj::set_define{ "debuggame", "EDITOR", "1" },
		prj::set_jobs{ "debuggame", 3 },
	};

	json::value external = json::parse(k_hand_user);
	external.as_object()["configurePresets"].as_array()[1].as_object()["cacheVariables"].as_object()["EXTERNAL"] = "2";
	external.as_object()["configurePresets"].as_array().emplace_back(json::parse(R"({ "name": "added", "inherits": "release" })"));
	dir.write("CMakeUserPresets.json", prj::to_indented_json(external));

	const auto saved = prj::save_user_presets(dir.path(), edits);
	BOOST_REQUIRE_MESSAGE(saved.error.empty(), saved.error);
	BOOST_TEST(saved.written);
	const auto read = prj::read_user_presets(dir.path());
	BOOST_REQUIRE(read.error.empty());
	const auto defines = prj::user_defines(read.doc, "debuggame");
	BOOST_TEST(defines.at("EXTERNAL") == "2");
	BOOST_TEST(defines.at("EDITOR") == "1");
	BOOST_TEST(defines.at("KEEP") == "yes");
	BOOST_TEST((prj::user_jobs(read.doc, "debuggame") == 3));
	BOOST_TEST(presets_named(read.doc, "configurePresets", "added") == 1u);
}

BOOST_AUTO_TEST_CASE(Save_CrlfFile_StaysCrlf)
{
	test_util::temp_dir dir("prj_presets_crlf");
	dir.write("CMakePresets.json", k_main);
	const fs::path user = dir.write("CMakeUserPresets.json", "{\r\n    \"version\": 3\r\n}\r\n");

	const auto saved = prj::save_user_presets(dir.path(), { prj::ensure_user_preset{ "release" } });
	BOOST_REQUIRE_MESSAGE(saved.error.empty(), saved.error);
	BOOST_TEST(saved.written);
	const std::string text = read_text(user);
	std::size_t lone_newlines = 0;
	for (std::size_t i = 0; i < text.size(); ++i) {
		lone_newlines += text[i] == '\n' && (i == 0 || text[i - 1] != '\r') ? 1 : 0;
	}
	BOOST_TEST(text.find("\r\n") != std::string::npos);
	BOOST_TEST(lone_newlines == 0u);
}

BOOST_AUTO_TEST_SUITE_END()
