#include <boost/test/unit_test.hpp>
#include "edt_template_ids.h"
#include <snk/project/prj_create.h>
#include <snk/resource/res_asset_id.h>
#include <snk/resource/res_asset_meta.h>
#include "test_temp_dir.hpp"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <string_view>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#ifndef SNK_EDITOR_DATA_DIR
#error "SNK_EDITOR_DATA_DIR must point at Editor/data"
#endif

// #765 (EPIC-57 section 6.1): a project made from a template gets asset ids of its own.

namespace {
	namespace fs = std::filesystem;
	namespace json = boost::json;

	const fs::path k_empty_template = fs::path{ SNK_EDITOR_DATA_DIR } / "templates" / "projects" / "empty";

	constexpr std::string_view k_a_guid = "11111111-1111-4111-8111-111111111111";
	constexpr std::string_view k_c_guid = "22222222-2222-4222-8222-222222222222";

	std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	std::string sidecar_text(std::string_view guid)
	{
		return res::write_asset_meta(res::asset_meta{ *res::asset_id_from_text(guid), "desc", {}, { "kept" } });
	}

	// a.desc names itself by a root __guid, c.desc by its sidecar; d.desc refers to both.
	void write_mini_template(const test_util::temp_dir& dir)
	{
		dir.write("tpl/project.snkproj", R"({"format_version": 1, "name": "Template", "engine_version": "0.0.0", "content_roots": ["res"]})");
		dir.write("tpl/res/a.desc", std::string{ R"({"__type": "a_desc", "__version": 1, "__guid": ")" } + std::string{ k_a_guid } + "\"}");
		dir.write("tpl/res/b.png", "not really a png");
		dir.write("tpl/res/c.desc", R"({"__type": "c_desc", "__version": 1})");
		dir.write("tpl/res/c.desc.resource", sidecar_text(k_c_guid));
		dir.write("tpl/res/sub/d.desc", std::string{ R"({"__type": "d_desc", "__version": 1, "items": [)" }
			+ R"({"guid": ")" + std::string{ k_c_guid } + R"(", "path": "res://c.desc"}, )"
			+ R"({"guid": ")" + std::string{ k_a_guid } + R"(", "path": "res://a.desc"}], )"
			+ R"("note": {"guid": ")" + std::string{ k_c_guid } + R"(", "label": "not a reference"}})");
		dir.write("tpl/res/.hidden/e.txt", "hidden");
		dir.write("tpl/code/main.cpp", "int main() {}");
	}

	prj::create_result create_with_fresh_ids(const fs::path& template_dir, const fs::path& target)
	{
		return prj::create_project(template_dir, target, "Game",
			[](const fs::path& staging) { return edt::give_template_fresh_ids(staging); });
	}

	res::asset_id sidecar_id(const fs::path& file)
	{
		fs::path sidecar = file;
		sidecar += ".resource";
		const auto parsed = res::read_asset_meta(read_text(sidecar));
		BOOST_REQUIRE_MESSAGE(parsed.meta.has_value(), "no valid sidecar for " << file.generic_string() << ": " << parsed.error.message);
		return parsed.meta->guid;
	}

	std::string string_at(const json::value& root, std::initializer_list<std::string_view> keys, std::size_t index = 0, bool in_items = false)
	{
		const json::value* value = &root;
		if (in_items) {
			value = &value->as_object().at("items").as_array().at(index);
		}
		for (const std::string_view key : keys) {
			value = &value->as_object().at(key);
		}
		return std::string(value->as_string().c_str());
	}

	std::size_t entries_in(const fs::path& folder)
	{
		std::size_t count = 0;
		for ([[maybe_unused]] const auto& entry : fs::directory_iterator(folder)) ++count;
		return count;
	}
}

BOOST_AUTO_TEST_SUITE(EdtTemplateIdsTests)

BOOST_AUTO_TEST_CASE(Create_TwiceFromOneTemplate_GivesEachProjectIdsOfItsOwnAndRewritesTheReferences)
{
	test_util::temp_dir dir("edt_template_ids");
	write_mini_template(dir);

	res::asset_id ids[2][4];
	const char* const names[] = { "a.desc", "b.png", "c.desc", "sub/d.desc" };
	for (int project = 0; project < 2; ++project) {
		const fs::path root = dir.path() / (project == 0 ? "one" : "two");
		const auto created = create_with_fresh_ids(dir.path() / "tpl", root);
		BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);

		for (int file = 0; file < 4; ++file) {
			BOOST_TEST_INFO("file: " << names[file]);
			ids[project][file] = sidecar_id(root / "res" / names[file]);
			BOOST_TEST(!ids[project][file].is_nil());
		}
		BOOST_TEST(res::to_text(ids[project][0]) != k_a_guid);
		BOOST_TEST(res::to_text(ids[project][2]) != k_c_guid);
		// The sidecar of the template keeps what it said besides the id.
		const auto c_meta = res::read_asset_meta(read_text(root / "res" / "c.desc.resource"));
		BOOST_REQUIRE(c_meta.meta.has_value());
		BOOST_TEST(c_meta.meta->importer == "desc");
		BOOST_REQUIRE(c_meta.meta->flags.size() == 1u);
		BOOST_TEST(c_meta.meta->flags[0] == "kept");

		const json::value a = json::parse(read_text(root / "res" / "a.desc"));
		BOOST_TEST(string_at(a, { "__guid" }) == res::to_text(ids[project][0]));
		const json::value d = json::parse(read_text(root / "res" / "sub" / "d.desc"));
		BOOST_TEST(string_at(d, { "guid" }, 0, true) == res::to_text(ids[project][2]));
		BOOST_TEST(string_at(d, { "path" }, 0, true) == "res://c.desc");
		BOOST_TEST(string_at(d, { "guid" }, 1, true) == res::to_text(ids[project][0]));
		// Not a reference: an object with a key besides guid and path.
		BOOST_TEST(string_at(d, { "note", "guid" }) == k_c_guid);

		BOOST_TEST(!fs::exists(root / "res" / ".hidden" / "e.txt.resource"));
		BOOST_TEST(!fs::exists(root / "code" / "main.cpp.resource")); // not under a content root
	}
	for (int file = 0; file < 4; ++file) {
		BOOST_TEST_INFO("file: " << names[file]);
		BOOST_TEST((ids[0][file] != ids[1][file]));
	}
	// The template itself is left as it was.
	BOOST_TEST(!fs::exists(dir.path() / "tpl" / "res" / "b.png.resource"));
	BOOST_TEST(read_text(dir.path() / "tpl" / "res" / "c.desc.resource") == sidecar_text(k_c_guid));
}

BOOST_AUTO_TEST_CASE(Create_FromATemplateWithAMalformedSidecar_FailsNamingItAndLeavesNothing)
{
	test_util::temp_dir dir("edt_template_ids_malformed");
	write_mini_template(dir);
	dir.write("tpl/res/broken.png", "x");
	dir.write("tpl/res/broken.png.resource", "{ not json");

	const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

	BOOST_TEST(created.error.find("res/broken.png.resource") != std::string::npos);
	BOOST_TEST(created.manifest_file.empty());
	BOOST_TEST(!fs::exists(dir.path() / "game"));
	BOOST_TEST(entries_in(dir.path()) == 1u); // the template only: no staging folder left
}

BOOST_AUTO_TEST_CASE(Create_FromATemplateWithABrokenDesc_KeepsItAndStillGivesItAnId)
{
	test_util::temp_dir dir("edt_template_ids_broken_desc");
	write_mini_template(dir);
	dir.write("tpl/res/broken.desc", "{ not json");

	const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
	BOOST_TEST(read_text(dir.path() / "game" / "res" / "broken.desc") == "{ not json");
	BOOST_TEST(!sidecar_id(dir.path() / "game" / "res" / "broken.desc").is_nil());
}

BOOST_AUTO_TEST_CASE(Create_FromTheShippedTemplate_GivesItsDescsSidecars)
{
	test_util::temp_dir dir("edt_template_ids_shipped");

	const auto created = create_with_fresh_ids(k_empty_template, dir.path() / "game");

	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
	BOOST_TEST(!sidecar_id(dir.path() / "game" / "res" / "input" / "actions.desc").is_nil());
	BOOST_TEST(!sidecar_id(dir.path() / "game" / "res" / "levels" / "main.desc").is_nil());
	BOOST_TEST(!fs::exists(dir.path() / "game" / "code" / "CMakeLists.txt.resource"));
	BOOST_TEST(!fs::exists(k_empty_template / "res" / "levels" / "main.desc.resource"));
}

BOOST_AUTO_TEST_SUITE_END()
