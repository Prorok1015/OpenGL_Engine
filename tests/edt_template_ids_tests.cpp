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
	constexpr std::string_view k_manifest = R"({"format_version": 1, "name": "Template", "engine_version": "0.0.0", "content_roots": ["res"]})";

	constexpr std::string_view k_a_guid = "11111111-1111-4111-8111-111111111111";
	constexpr std::string_view k_c_guid = "22222222-2222-4222-8222-222222222222";
	constexpr std::string_view k_e_guid = "33333333-3333-4333-8333-333333333333";
	constexpr std::string_view k_e_root_guid = "44444444-4444-4444-8444-444444444444";

	std::string read_text(const fs::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	}

	std::string sidecar_text(std::string_view guid)
	{
		return res::write_asset_meta(res::asset_meta{ *res::asset_id_from_text(guid), "desc", {}, { "kept" } });
	}

	std::string reference(std::string_view guid, std::string_view path)
	{
		return std::string{ R"({"guid": ")" } + std::string{ guid } + R"(", "path": ")" + std::string{ path } + "\"}";
	}

	std::string desc_with_guid(std::string_view type, std::string_view guid)
	{
		return std::string{ R"({"__type": ")" } + std::string{ type } + R"(", "__version": 1, "__guid": ")" + std::string{ guid } + "\"}";
	}

	// a.desc names itself by a root __guid, c.desc by its sidecar, e.desc by both (they differ), f.desc by a __guid that
	// is c's; d.desc refers to a and c.
	void write_mini_template(const test_util::temp_dir& dir)
	{
		dir.write("tpl/project.snkproj", k_manifest);
		dir.write("tpl/res/a.desc", desc_with_guid("a_desc", k_a_guid));
		dir.write("tpl/res/b.png", "not really a png");
		dir.write("tpl/res/c.desc", R"({"__type": "c_desc", "__version": 1})");
		dir.write("tpl/res/c.desc.resource", sidecar_text(k_c_guid));
		dir.write("tpl/res/sub/d.desc", std::string{ R"({"__type": "d_desc", "__version": 1, "items": [)" }
			+ reference(k_c_guid, "res://c.desc") + ", " + reference(k_a_guid, "res://a.desc") + "], "
			+ R"("note": {"guid": ")" + std::string{ k_c_guid } + R"(", "label": "not a reference"}, )"
			+ R"("lonely": {"guid": ")" + std::string{ k_c_guid } + R"("}})");
		dir.write("tpl/res/e.desc", desc_with_guid("e_desc", k_e_root_guid));
		dir.write("tpl/res/e.desc.resource", sidecar_text(k_e_guid));
		dir.write("tpl/res/f.desc", desc_with_guid("f_desc", k_c_guid));
		dir.write("tpl/res/.hidden/h.txt", "hidden");
		dir.write("tpl/res/.gitkeep", "");
		dir.write("tpl/res/Thumbs.db", "x");
		dir.write("tpl/res/scratch.tmp", "x");
		dir.write("tpl/code/main.cpp", "int main() {}");
	}

	prj::create_result create_with_fresh_ids(const fs::path& template_dir, const fs::path& target)
	{
		return prj::create_project(template_dir, target, "Game",
			[](const fs::path& staging) { return edt::give_template_fresh_ids(staging); });
	}

	res::asset_id id_in(const fs::path& sidecar)
	{
		const auto parsed = res::read_asset_meta(read_text(sidecar));
		BOOST_REQUIRE_MESSAGE(parsed.meta.has_value(), "no valid sidecar " << sidecar.generic_string() << ": " << parsed.error.message);
		return parsed.meta->guid;
	}

	res::asset_id sidecar_id(const fs::path& file)
	{
		fs::path sidecar = file;
		sidecar += ".resource";
		return id_in(sidecar);
	}

	const json::value& at(const json::value& root, std::initializer_list<std::string_view> keys)
	{
		const json::value* value = &root;
		for (const std::string_view key : keys) {
			value = &value->as_object().at(key);
		}
		return *value;
	}

	std::string text_at(const json::value& root, std::initializer_list<std::string_view> keys)
	{
		return std::string(at(root, keys).as_string().c_str());
	}

	std::string item_guid(const json::value& root, std::size_t index)
	{
		return std::string(at(root, { "items" }).as_array().at(index).as_object().at("guid").as_string().c_str());
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

	constexpr int k_files = 6;
	const char* const names[k_files] = { "a.desc", "b.png", "c.desc", "sub/d.desc", "e.desc", "f.desc" };
	res::asset_id ids[2][k_files];
	for (int project = 0; project < 2; ++project) {
		const fs::path root = dir.path() / (project == 0 ? "one" : "two");
		const auto created = create_with_fresh_ids(dir.path() / "tpl", root);
		BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);

		for (int file = 0; file < k_files; ++file) {
			BOOST_TEST_INFO("file: " << names[file]);
			ids[project][file] = sidecar_id(root / "res" / names[file]);
			BOOST_TEST(!ids[project][file].is_nil());
		}
		const auto& [a, b, c, d, e, f] = ids[project];
		BOOST_TEST(res::to_text(a) != k_a_guid);
		BOOST_TEST(res::to_text(c) != k_c_guid);
		BOOST_TEST(res::to_text(e) != k_e_guid);
		BOOST_TEST((f != c));
		// The sidecar of the template keeps what it said besides the id.
		const auto c_meta = res::read_asset_meta(read_text(root / "res" / "c.desc.resource"));
		BOOST_REQUIRE(c_meta.meta.has_value());
		BOOST_TEST(c_meta.meta->importer == "desc");
		BOOST_REQUIRE(c_meta.meta->flags.size() == 1u);
		BOOST_TEST(c_meta.meta->flags[0] == "kept");

		// The root __guid of every desc is the id of its own sidecar.
		BOOST_TEST(text_at(json::parse(read_text(root / "res" / "a.desc")), { "__guid" }) == res::to_text(a));
		BOOST_TEST(text_at(json::parse(read_text(root / "res" / "e.desc")), { "__guid" }) == res::to_text(e));
		BOOST_TEST(text_at(json::parse(read_text(root / "res" / "f.desc")), { "__guid" }) == res::to_text(f));

		const json::value doc = json::parse(read_text(root / "res" / "sub" / "d.desc"));
		BOOST_TEST(item_guid(doc, 0) == res::to_text(c)); // c's id, not f's whose __guid was the same
		BOOST_TEST(item_guid(doc, 1) == res::to_text(a));
		// Not references: a key besides guid and path, or no path.
		BOOST_TEST(text_at(doc, { "note", "guid" }) == k_c_guid);
		BOOST_TEST(text_at(doc, { "lonely", "guid" }) == k_c_guid);

		for (const char* ignored : { ".hidden/h.txt", ".gitkeep", "Thumbs.db", "scratch.tmp" }) {
			BOOST_TEST_INFO("ignored: " << ignored);
			fs::path sidecar = root / "res" / ignored;
			sidecar += ".resource";
			BOOST_TEST(!fs::exists(sidecar));
		}
		BOOST_TEST(!fs::exists(root / "code" / "main.cpp.resource")); // not under a content root
		(void)b;
		(void)d;
	}
	for (int file = 0; file < k_files; ++file) {
		BOOST_TEST_INFO("file: " << names[file]);
		BOOST_TEST((ids[0][file] != ids[1][file]));
	}
	// The template itself is left as it was.
	BOOST_TEST(!fs::exists(dir.path() / "tpl" / "res" / "b.png.resource"));
	BOOST_TEST(read_text(dir.path() / "tpl" / "res" / "c.desc.resource") == sidecar_text(k_c_guid));
}

BOOST_AUTO_TEST_CASE(Create_FromATemplateWithTwoSidecarsOfOneId_GivesEachItsOwnAndReferencesGoToTheFirst)
{
	test_util::temp_dir dir("edt_template_ids_repeated");
	dir.write("tpl/project.snkproj", k_manifest);
	dir.write("tpl/res/p1.png", "1");
	dir.write("tpl/res/p1.png.resource", sidecar_text(k_c_guid));
	dir.write("tpl/res/p2.png", "2");
	dir.write("tpl/res/p2.png.resource", sidecar_text(k_c_guid));
	dir.write("tpl/res/r.desc", std::string{ R"({"__type": "r_desc", "__version": 1, "items": [)" } + reference(k_c_guid, "res://p2.png") + "]}");

	const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
	const res::asset_id p1 = sidecar_id(dir.path() / "game" / "res" / "p1.png");
	const res::asset_id p2 = sidecar_id(dir.path() / "game" / "res" / "p2.png");
	BOOST_TEST((p1 != p2));
	BOOST_TEST(res::to_text(p1) != k_c_guid);
	BOOST_TEST(item_guid(json::parse(read_text(dir.path() / "game" / "res" / "r.desc")), 0) == res::to_text(p1));
}

BOOST_AUTO_TEST_CASE(Create_FromATemplateWithASidecarInOtherLetterCase_GivesTheFileNoSecondSidecar)
{
	test_util::temp_dir dir("edt_template_ids_case");
	dir.write("tpl/project.snkproj", k_manifest);
	dir.write("tpl/res/a.png", "x");
	dir.write("tpl/res/a.png.RESOURCE", sidecar_text(k_c_guid));
	dir.write("tpl/res/r.desc", std::string{ R"({"__type": "r_desc", "__version": 1, "items": [)" } + reference(k_c_guid, "res://a.png") + "]}");

	const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
	std::size_t sidecars_of_a = 0;
	fs::path sidecar;
	for (const auto& entry : fs::directory_iterator(dir.path() / "game" / "res")) {
		std::string name = entry.path().filename().string();
		for (char& c : name) {
			c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}
		if (name == "a.png.resource") {
			++sidecars_of_a;
			sidecar = entry.path();
		}
	}
	BOOST_REQUIRE(sidecars_of_a == 1u);
	const res::asset_id a = id_in(sidecar);
	BOOST_TEST(res::to_text(a) != k_c_guid);
	BOOST_TEST(item_guid(json::parse(read_text(dir.path() / "game" / "res" / "r.desc")), 0) == res::to_text(a));
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

BOOST_AUTO_TEST_CASE(Create_FromATemplateWithABrokenOrHugeDesc_FailsNamingItAndLeavesNothing)
{
	for (const bool huge : { false, true }) {
		BOOST_TEST_INFO("huge: " << huge);
		test_util::temp_dir dir("edt_template_ids_bad_desc");
		write_mini_template(dir);
		dir.write("tpl/res/bad.desc", huge ? std::string(4u * 1024u * 1024u + 1u, ' ') + "{}" : std::string{ "{ not json" });

		const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

		BOOST_TEST(created.error.find("res/bad.desc") != std::string::npos);
		BOOST_TEST(created.manifest_file.empty());
		BOOST_TEST(!fs::exists(dir.path() / "game"));
		BOOST_TEST(entries_in(dir.path()) == 1u);
	}
}

#if !defined(_WIN32)
BOOST_AUTO_TEST_CASE(Create_FromATemplateWhoseContentRootIsALink_FailsAndWritesNothingThere)
{
	test_util::temp_dir dir("edt_template_ids_link_root");
	dir.write("tpl/project.snkproj", k_manifest);
	dir.write("outside/x.png", "x");
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "tpl" / "res");

	const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

	BOOST_TEST(created.error.find("link") != std::string::npos);
	BOOST_TEST(!fs::exists(dir.path() / "game"));
	BOOST_TEST(entries_in(dir.path() / "outside") == 1u);
	BOOST_TEST(entries_in(dir.path()) == 2u); // tpl and outside: no staging folder left
}

BOOST_AUTO_TEST_CASE(Create_FromATemplateWithALinkedFolderInside_LeavesTheLinkTargetAlone)
{
	test_util::temp_dir dir("edt_template_ids_link_inside");
	dir.write("tpl/project.snkproj", k_manifest);
	dir.write("tpl/res/own.png", "x");
	dir.write("outside/x.png", "x");
	fs::create_directory_symlink(dir.path() / "outside", dir.path() / "tpl" / "res" / "linked");

	const auto created = create_with_fresh_ids(dir.path() / "tpl", dir.path() / "game");

	BOOST_REQUIRE_MESSAGE(created.error.empty(), created.error);
	BOOST_TEST(!sidecar_id(dir.path() / "game" / "res" / "own.png").is_nil());
	BOOST_TEST(entries_in(dir.path() / "outside") == 1u);
}
#endif

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
